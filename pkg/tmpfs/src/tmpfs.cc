/*
 * TuringOS tmpfs — in-process RAM filesystem mounted at /tmp
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Many libc paths implicitly need /tmp (tmpfile(), mkstemp(), library scratch
 * files).  This is a per-process memory-backed filesystem grafted onto the
 * process's own VFS mount tree at /tmp, so those calls work without a disk or
 * a server.  It is linked into apps that want /tmp (like the ext4 client lib);
 * a static initialiser mounts it after VFS init.
 *
 *   Tmpfs_inode — refcounted file content (growable heap buffer), shared by
 *                 every open handle of a path.
 *   Tmpfs_file  — per-open handle (Be_file_pos, own position) delegating to
 *                 the shared inode.  A fresh one is returned per open so two
 *                 opens of the same file don't share a read/write position.
 *   Tmpfs_dir   — directory; children in a linked list, supports
 *                 create/mkdir/unlink/rmdir/getdents.
 */

#include <l4/l4re_vfs/backend>
#include <l4/cxx/ref_ptr>
#include <l4/crtn/initpriorities.h>

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <new>

namespace {

using L4Re::Vfs::File;
using L4Re::Vfs::Be_file;
using L4Re::Vfs::Be_file_pos;
using cxx::Ref_ptr;

// ---------------------------------------------------------------------------
// Tmpfs_inode — the shared, refcounted content of a regular file.
// ---------------------------------------------------------------------------
class Tmpfs_inode
{
public:
  void add_ref() noexcept    { ++_ref; }
  int  remove_ref() noexcept { return --_ref; }

  ~Tmpfs_inode() noexcept { free(_data); }

  size_t size() const noexcept { return _size; }

  ssize_t pread(void *dst, size_t len, off64_t off) noexcept
  {
    if (off < 0 || (size_t)off >= _size) return 0;
    size_t avail = _size - (size_t)off;
    if (len > avail) len = avail;
    memcpy(dst, _data + off, len);
    return (ssize_t)len;
  }

  ssize_t pwrite(const void *src, size_t len, off64_t off) noexcept
  {
    if (off < 0) return -EINVAL;
    if (!len) return 0;
    if (!ensure((size_t)off + len)) return -ENOSPC;
    if ((size_t)off > _size)             // write past EOF: zero the hole
      memset(_data + _size, 0, (size_t)off - _size);
    memcpy(_data + off, src, len);
    if ((size_t)off + len > _size) _size = (size_t)off + len;
    return (ssize_t)len;
  }

  int truncate(off64_t len) noexcept
  {
    if (len < 0) return -EINVAL;
    if ((size_t)len > _size)
      {
        if (!ensure((size_t)len)) return -ENOSPC;
        memset(_data + _size, 0, (size_t)len - _size);
      }
    _size = (size_t)len;
    return 0;
  }

private:
  bool ensure(size_t need) noexcept
  {
    if (need <= _cap) return true;
    size_t ncap = _cap ? _cap : 64;
    while (ncap < need) ncap *= 2;
    char *p = static_cast<char *>(realloc(_data, ncap));
    if (!p) return false;
    _data = p;
    _cap  = ncap;
    return true;
  }

  int    _ref  = 0;
  char  *_data = nullptr;
  size_t _size = 0;
  size_t _cap  = 0;
};

// ---------------------------------------------------------------------------
// Tmpfs_file — a per-open handle over a shared inode.  Position lives here
// (Be_file_pos::_pos), so each open() gets an independent cursor.
// ---------------------------------------------------------------------------
class Tmpfs_file : public Be_file_pos
{
public:
  Tmpfs_file(Ref_ptr<Tmpfs_inode> const &ino, bool append) noexcept
  : Be_file_pos(), _ino(ino), _append(append) {}

  off64_t size() const noexcept override { return (off64_t)_ino->size(); }

  ssize_t preadv(const struct iovec *iov, int cnt, off64_t off) noexcept override
  {
    ssize_t total = 0;
    for (int i = 0; i < cnt; ++i)
      {
        ssize_t r = _ino->pread(iov[i].iov_base, iov[i].iov_len, off);
        if (r <= 0) break;
        off   += r;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;   // hit EOF
      }
    return total;
  }

  ssize_t pwritev(const struct iovec *iov, int cnt, off64_t off) noexcept override
  {
    if (_append) off = (off64_t)_ino->size();   // O_APPEND: write at EOF
    ssize_t total = 0;
    for (int i = 0; i < cnt; ++i)
      {
        if (!iov[i].iov_len) continue;
        ssize_t r = _ino->pwrite(iov[i].iov_base, iov[i].iov_len, off);
        if (r < 0) return total ? total : r;
        off   += r;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
      }
    return total;
  }

  int ftruncate(off64_t len) noexcept override { return _ino->truncate(len); }

  int fstat(struct stat64 *b) const noexcept override
  {
    memset(b, 0, sizeof(*b));
    b->st_mode    = S_IFREG | 0644;
    b->st_size    = (off64_t)_ino->size();
    b->st_blksize = 4096;
    b->st_blocks  = (b->st_size + 511) / 512;
    return 0;
  }

  int get_status_flags() const noexcept override { return O_RDWR; }
  int set_status_flags(long) noexcept override   { return 0; }
  int fsync() const noexcept override            { return 0; }

private:
  Ref_ptr<Tmpfs_inode> _ino;
  bool                  _append = false;
};

// ---------------------------------------------------------------------------
// Tmpfs_dir — a directory holding named children (files or sub-directories).
// ---------------------------------------------------------------------------
class Tmpfs_dir : public Be_file
{
public:
  Tmpfs_dir() noexcept : Be_file() {}
  ~Tmpfs_dir() noexcept
  {
    Entry *e = _head;
    while (e) { Entry *n = e->next; delete e; e = n; }
  }

  int get_entry(const char *path, int flags, mode_t,
                Ref_ptr<File> *f) noexcept override
  {
    const char *name; size_t n; const char *rest;
    if (!split(path, &name, &n, &rest))
      { *f = cxx::ref_ptr(this); return 0; }     // resolves to this dir

    Entry *e = find(name, n);

    if (rest)                                     // intermediate component
      {
        if (!e)         return -ENOENT;
        if (!e->sub)    return -ENOTDIR;
        return e->sub->get_entry(rest, flags, 0, f);
      }

    bool append = (flags & O_APPEND) != 0;

    if (e)                                         // leaf exists
      {
        if (e->sub) { *f = e->sub; return 0; }
        if (flags & O_TRUNC) e->ino->truncate(0);
        *f = cxx::make_ref_obj<Tmpfs_file>(e->ino, append); // fresh handle
        return *f ? 0 : -ENOMEM;
      }

    if (!(flags & O_CREAT)) return -ENOENT;        // leaf missing

    Ref_ptr<Tmpfs_inode> ino = cxx::make_ref_obj<Tmpfs_inode>();
    if (!ino || !add_file(name, n, ino)) return -ENOMEM;
    *f = cxx::make_ref_obj<Tmpfs_file>(ino, append);
    return *f ? 0 : -ENOMEM;
  }

  int mkdir(const char *path, mode_t) noexcept override
  {
    const char *name; size_t n; const char *rest;
    if (!split(path, &name, &n, &rest)) return -EEXIST;
    Entry *e = find(name, n);
    if (rest)
      {
        if (!e)      return -ENOENT;
        if (!e->sub) return -ENOTDIR;
        return e->sub->mkdir(rest, 0);
      }
    if (e) return -EEXIST;
    Ref_ptr<Tmpfs_dir> nd = cxx::make_ref_obj<Tmpfs_dir>();
    if (!nd || !add_dir(name, n, nd)) return -ENOMEM;
    return 0;
  }

  int unlink(const char *path) noexcept override
  {
    const char *name; size_t n; const char *rest;
    if (!split(path, &name, &n, &rest)) return -EISDIR;
    Entry *e = find(name, n);
    if (!e) return -ENOENT;
    if (rest)
      {
        if (!e->sub) return -ENOTDIR;
        return e->sub->unlink(rest);
      }
    if (e->sub) return -EISDIR;
    remove(e);
    return 0;
  }

  int rmdir(const char *path) noexcept override
  {
    const char *name; size_t n; const char *rest;
    if (!split(path, &name, &n, &rest)) return -EINVAL;
    Entry *e = find(name, n);
    if (!e) return -ENOENT;
    if (rest)
      {
        if (!e->sub) return -ENOTDIR;
        return e->sub->rmdir(rest);
      }
    if (!e->sub) return -ENOTDIR;
    if (e->sub->_head) return -ENOTEMPTY;
    remove(e);
    return 0;
  }

  int faccessat(const char *path, int, int) noexcept override
  {
    const char *name; size_t n; const char *rest;
    if (!split(path, &name, &n, &rest)) return 0;
    Entry *e = find(name, n);
    if (!e) return -ENOENT;
    if (rest)
      {
        if (!e->sub) return -ENOTDIR;
        return e->sub->faccessat(rest, 0, 0);
      }
    return 0;
  }

  int fstat(struct stat64 *b) const noexcept override
  {
    memset(b, 0, sizeof(*b));
    b->st_mode    = S_IFDIR | 0755;
    b->st_nlink   = 2;
    b->st_blksize = 4096;
    return 0;
  }

  ssize_t getdents(char *buf, size_t sz) noexcept override
  {
    struct dirent64 *d = reinterpret_cast<struct dirent64 *>(buf);
    ssize_t ret = 0;

    Entry *e = _head;
    for (unsigned i = 0; i < _dent_pos && e; ++i) e = e->next;

    while (e)
      {
        size_t nl = strlen(e->name) + 1;
        if (nl > sizeof(d->d_name)) nl = sizeof(d->d_name);
        unsigned reclen = offsetof(struct dirent64, d_name) + nl;
        reclen = (reclen + sizeof(long) - 1) & ~(sizeof(long) - 1);
        if (reclen > sz) break;

        d->d_ino    = 1;
        d->d_off    = 0;
        d->d_reclen = (unsigned short)reclen;
        d->d_type   = e->sub ? DT_DIR : DT_REG;
        memcpy(d->d_name, e->name, nl - 1);
        d->d_name[nl - 1] = 0;

        ret += reclen;
        sz  -= reclen;
        d = reinterpret_cast<struct dirent64 *>(
              reinterpret_cast<char *>(d) + reclen);
        e = e->next;
        ++_dent_pos;
      }

    if (!ret) _dent_pos = 0;   // end of stream; reset for the next opendir
    return ret;
  }

private:
  struct Entry
  {
    char                *name = nullptr;
    Ref_ptr<Tmpfs_inode> ino;          // set for regular files
    Ref_ptr<Tmpfs_dir>   sub;          // set for sub-directories
    Entry               *next = nullptr;
    ~Entry() { free(name); }
  };

  static bool split(const char *path, const char **name, size_t *n,
                    const char **rest) noexcept
  {
    while (*path == '/') ++path;
    if (!*path) return false;
    const char *sep = strchr(path, '/');
    *name = path;
    if (sep)
      {
        *n = (size_t)(sep - path);
        while (*sep == '/') ++sep;
        *rest = *sep ? sep : nullptr;   // trailing slash => treat as leaf
      }
    else
      {
        *n    = strlen(path);
        *rest = nullptr;
      }
    return true;
  }

  Entry *find(const char *name, size_t n) noexcept
  {
    for (Entry *e = _head; e; e = e->next)
      if (strlen(e->name) == n && memcmp(e->name, name, n) == 0)
        return e;
    return nullptr;
  }

  Entry *alloc(const char *name, size_t n) noexcept
  {
    Entry *e = new (std::nothrow) Entry();
    if (!e) return nullptr;
    e->name = static_cast<char *>(malloc(n + 1));
    if (!e->name) { delete e; return nullptr; }
    memcpy(e->name, name, n);
    e->name[n] = 0;
    e->next = _head;
    _head   = e;
    return e;
  }

  bool add_file(const char *name, size_t n, Ref_ptr<Tmpfs_inode> const &ino) noexcept
  {
    Entry *e = alloc(name, n);
    if (!e) return false;
    e->ino = ino;
    return true;
  }

  bool add_dir(const char *name, size_t n, Ref_ptr<Tmpfs_dir> const &sub) noexcept
  {
    Entry *e = alloc(name, n);
    if (!e) return false;
    e->sub = sub;
    return true;
  }

  void remove(Entry *target) noexcept
  {
    Entry **pp = &_head;
    while (*pp && *pp != target) pp = &(*pp)->next;
    if (*pp) { *pp = target->next; delete target; }
  }

  Entry   *_head     = nullptr;
  unsigned _dent_pos = 0;
};

// ---------------------------------------------------------------------------
// Mount /tmp once, after VFS init.
// ---------------------------------------------------------------------------
struct Tmpfs_init
{
  Tmpfs_init()
  {
    Ref_ptr<File> root = cxx::make_ref_obj<Tmpfs_dir>();
    // Mount path is relative to the VFS root with no leading slash (matching
    // devfs's mount("dev", ...)); a leading slash makes Vfs::mount's
    // strip_first() see an empty first component and bail with -EEXIST.
    if (root && L4Re::Vfs::vfs_ops)
      L4Re::Vfs::vfs_ops->mount("tmp", root);
  }
} _tmpfs_init __attribute__((init_priority(INIT_PRIO_LATE + 1)));

} // namespace

// Referenced by linkers via `-u tmpfs_module_init` so this TU (and its static
// initialiser above) is pulled in from the static library.
extern "C" void tmpfs_module_init() {}
