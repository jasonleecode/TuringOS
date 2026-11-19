/*
 * TuringOS ext4fs — client-side VFS file wrapper + factory (Phase 4)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */

#include "ext4_vfs.h"

#include <l4/re/env>
#include <l4/re/rm>
#include <l4/re/cap_alloc>
#include <l4/crtn/initpriorities.h>

#include <string.h>
#include <cstdio>
#include <initializer_list>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

// --------------------------------------------------------------------------
// Ext4_file_vfs implementation
// --------------------------------------------------------------------------

Ext4_file_vfs::Ext4_file_vfs(L4::Cap<Ext4_file_ops> cap) noexcept
: Be_file_pos(), _cap(cap)
{
  // Allocate a local cap slot for the bounce buffer DS the server hands back.
  _buf = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
  if (!_buf.is_valid())
    return;

  // Open the file server-side and obtain the shared bounce buffer + size.
  l4_uint64_t fsize = 0;
  l4_uint32_t bsz   = 0;
  long r = cap->setup(_buf.get(), fsize, bsz);
  if (r < 0 || bsz == 0)
    return;

  _size     = fsize;
  _buf_size = bsz;

  // Map the bounce buffer locally (round up to a page).
  unsigned long map_sz = ((unsigned long)bsz + 4095UL) & ~4095UL;
  if (map_sz < 4096UL) map_sz = 4096UL;
  _buf_addr = 0;
  r = L4Re::Env::env()->rm()->attach(
        &_buf_addr, map_sz,
        L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
        L4::Ipc::make_cap_rw(_buf.get()), 0);
  if (r < 0)
    { _buf_addr = 0; return; }

  _valid = true;
}

Ext4_file_vfs::Ext4_file_vfs(L4Re::Util::Unique_cap<Ext4_file_ops> &slot) noexcept
: Ext4_file_vfs(slot.get())
{ _slot = cxx::move(slot); }

Ext4_file_vfs::~Ext4_file_vfs() noexcept
{
  if (_buf_addr)
    L4Re::Env::env()->rm()->detach(_buf_addr, nullptr);
  // _buf unique_cap frees the slot.

  // Tell the server this handle is gone so it can close the ext4 file and free
  // the per-open Ext4_file_svr (object + IPC gate + buffer DS).  Runs for
  // *every* open (read-only too) — without it the server leaks a cap slot per
  // open.  Best-effort: the cap is about to be dropped anyway.
  if (_cap.is_valid())
    _cap->release();
}

ssize_t Ext4_file_vfs::preadv(const struct iovec *iov, int cnt,
                               off64_t offset) noexcept
{
  if (!_valid || !_buf_addr) return -EIO;

  ssize_t total = 0;
  off64_t pos = offset;

  for (int i = 0; i < cnt; ++i)
    {
      char  *dst    = reinterpret_cast<char *>(iov[i].iov_base);
      size_t remain = iov[i].iov_len;

      while (remain > 0)
        {
          l4_uint32_t chunk = (remain > _buf_size) ? _buf_size
                                                   : (l4_uint32_t)remain;
          l4_uint32_t got = 0;
          long r = _cap->pread((l4_uint64_t)pos, chunk, got);
          if (r < 0)
            return total ? total : -EIO;
          if (got == 0)
            return total;                 // EOF

          memcpy(dst, reinterpret_cast<void *>(_buf_addr), got);
          dst    += got;
          remain -= got;
          pos    += (off64_t)got;
          total  += (ssize_t)got;

          if (got < chunk)
            return total;                 // short read => EOF reached
        }
    }
  return total;
}

ssize_t Ext4_file_vfs::pwritev(const struct iovec *iov, int cnt,
                                off64_t offset) noexcept
{
  if (!_valid || !_buf_addr) return -EIO;

  ssize_t total = 0;
  off64_t pos = offset;

  for (int i = 0; i < cnt; ++i)
    {
      const char *src    = reinterpret_cast<const char *>(iov[i].iov_base);
      size_t      remain = iov[i].iov_len;

      while (remain > 0)
        {
          l4_uint32_t chunk = (remain > _buf_size) ? _buf_size
                                                   : (l4_uint32_t)remain;
          memcpy(reinterpret_cast<void *>(_buf_addr), src, chunk);

          l4_uint32_t put = 0;
          long r;
          if (_append)
            {
              // Atomic server-side append: the offset is chosen by the server
              // at EOF, so concurrent appenders never overwrite each other.
              l4_uint64_t at = 0;
              r = _cap->pappend(chunk, put, at);
              if (r >= 0 && put > 0)
                pos = (off64_t)(at + put);   // fd advances past the appended data
            }
          else
            {
              r = _cap->pwrite((l4_uint64_t)pos, chunk, put);
              if (r >= 0 && put > 0)
                pos += (off64_t)put;
            }
          if (r < 0 || put == 0)
            return total ? total : -EIO;

          if ((l4_uint64_t)pos > _size)
            _size = (l4_uint64_t)pos;
          src    += put;
          remain -= put;
          total  += (ssize_t)put;

          if (put < chunk)
            return total;                 // partial write
        }
    }
  return total;
}

int Ext4_file_vfs::fstat(struct stat64 *buf) const noexcept
{
  memset(buf, 0, sizeof(*buf));
  buf->st_mode    = S_IFREG | 0644;
  buf->st_size    = (off64_t)_size;
  buf->st_blksize = 4096;
  buf->st_blocks  = (buf->st_size + 511) / 512;
  return 0;
}

int Ext4_file_vfs::ftruncate(off64_t pos) noexcept
{
  if (!_valid || pos < 0) return -EINVAL;
  long r = _cap->ftruncate((l4_uint64_t)pos);
  if (r < 0) return -EIO;
  _size = (l4_uint64_t)pos;
  return 0;
}

int Ext4_file_vfs::get_status_flags() const noexcept
{ return O_RDWR | (_append ? O_APPEND : 0); }

int Ext4_file_vfs::set_status_flags(long flags) noexcept
{
  _append = (flags & O_APPEND) != 0;
  return 0;
}

// --------------------------------------------------------------------------
// Factory registration — executed before main() via init_priority
// --------------------------------------------------------------------------

// This symbol is referenced by native_shell via LDFLAGS += -u ext4_client_module_init
// so that the linker includes this TU from the static library, which in turn
// causes the file_factory to be registered via the static constructors below.
extern "C" void ext4_client_module_init() {}

namespace {

// File_factory_t<Ext4_file_ops, Ext4_file_vfs> matches caps whose L4::Meta
// interface() returns protocol 0x5800 (Ext4_file_ops::Protocol) and creates
// an Ext4_file_vfs.
static L4Re::Vfs::File_factory_t<Ext4_file_ops, Ext4_file_vfs>
  ext4_file_factory
  __attribute__((init_priority(INIT_PRIO_LATE)));

// A POSIX directory rooted at the one namespace capability handed to this task.
class Shared_dir : public L4Re::Vfs::Be_file {
  using File = L4Re::Vfs::File;
  using Ref = cxx::Ref_ptr<File>;
  const char *mount_name;
  char prefix[256]{};
  L4::Cap<Ext4_dir_ops> ns;
  Ref listing;

  bool path(const char *name,char (&out)[256]) const {
    int n=snprintf(out,sizeof(out),"%s%s%s",prefix,*prefix?"/":"",name);
    return n>=0 && static_cast<size_t>(n)<sizeof(out);
  }
  int remove(const char *name,bool directory) {
    char p[256];
    if (!path(name,p)) return -ENAMETOOLONG;
    return ns->ext_remove({strlen(p),p},directory);
  }
public:
  Shared_dir(const char *mount,const char *relative,L4::Cap<Ext4_dir_ops> cap)
    : mount_name(mount),ns(cap) { strcpy(prefix,relative); }
  int fstat(struct stat64 *b) const noexcept override {
    memset(b,0,sizeof(*b)); b->st_mode=S_IFDIR|0755; return 0;
  }
  int mkdir(const char *name,mode_t) noexcept override {
    char p[256];
    if (!path(name,p)) return -ENAMETOOLONG;
    return ns->ext_mkdir({strlen(p),p});
  }
  int unlink(const char *name) noexcept override { return remove(name,false); }
  int rmdir(const char *name) noexcept override { return remove(name,true); }
  ssize_t getdents(char *buf,size_t size) noexcept override {
    if (!listing) {
      char raw[280];
      snprintf(raw,sizeof(raw),"%s%s%s",mount_name,*prefix?"/":"",prefix);
      int r=L4Re::Vfs::vfs_ops->get_root()->get_entry(raw,O_RDONLY,0,&listing);
      if (r<0) return r;
    }
    return listing->getdents(buf,size);
  }
  int get_entry(const char *name,int flags,mode_t,Ref *result) noexcept override {
    if (!*name || !strcmp(name,".")) {
      *result=cxx::make_ref_obj<Shared_dir>(mount_name,prefix,ns);
      return *result ? 0 : -ENOMEM;
    }
    char p[256];
    if (!path(name,p)) return -ENAMETOOLONG;
    auto slot=L4Re::Util::make_unique_cap<Ext4_file_ops>();
    if (!slot.is_valid()) return -ENOMEM;
    unsigned directory=0;
    L4::Ipc::Snd_fpage object;
    long rc=Ext4_dir_ops::ext_open_t::call(ns,L4::Ipc::Small_buf(slot.get()),L4::Ipc::Array<char const,unsigned long>(strlen(p),p),
                                            (unsigned)flags,directory,object);
    if (rc<0) return rc;
    if (directory) {
      *result=cxx::make_ref_obj<Shared_dir>(mount_name,p,ns);
      return *result ? 0 : -ENOMEM;
    }
    auto file = cxx::make_ref_obj<Ext4_file_vfs>(slot);
    if (!file) { slot->release(); return -ENOMEM; }
    if (!file->valid()) return -EIO;
    file->set_status_flags(flags);
    *result=file;
    return 0;
  }
};

// Registration wrapper: runs after VFS is initialised (INIT_PRIO_VFS_INIT),
// before any library constructors that might open files.
struct Ext4_vfs_init
{
  Ext4_vfs_init()
  {
    auto ptr = cxx::ref_ptr(&ext4_file_factory);
    L4Re::Vfs::vfs_ops->register_file_factory(ptr);
    for (auto name : {"ext4","tmp","data"}) {
      auto ns=L4Re::Env::env()->get_cap<Ext4_dir_ops>(name);
      if (ns.is_valid()) L4Re::Vfs::vfs_ops->mount(name,cxx::make_ref_obj<Shared_dir>(name,"",ns));
    }
    ptr.release(); // prevent deletion of the static object
  }
} _ext4_vfs_init __attribute__((init_priority(INIT_PRIO_LATE + 1)));

} // anonymous namespace
