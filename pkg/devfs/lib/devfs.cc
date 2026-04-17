#include <l4/devfs/devfs.h>

#include <l4/l4re_vfs/backend>

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

#include <map>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ */
/* Global device registry                                               */
/* ------------------------------------------------------------------ */

namespace {

struct Registry
{
  std::map<std::string, cxx::Ref_ptr<Devfs::Device_file>> devices;
  pthread_mutex_t lock;

  Registry() { pthread_mutex_init(&lock, nullptr); }
};

static Registry g_reg;

/* ------------------------------------------------------------------ */
/* Built-in devices: /dev/null, /dev/zero                              */
/* ------------------------------------------------------------------ */

class Null_file : public Devfs::Device_file
{
public:
  ssize_t readv(const struct iovec *, int) noexcept override
  { return 0; }

  ssize_t writev(const struct iovec *iov, int iovcnt) noexcept override
  {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i)
      total += (ssize_t)iov[i].iov_len;
    return total;
  }

  int fstat(struct stat64 *buf) const noexcept override
  {
    memset(buf, 0, sizeof(*buf));
    buf->st_mode  = S_IFCHR | 0666;
    buf->st_rdev  = makedev(1, 3); // major=1, minor=3 (Linux /dev/null)
    buf->st_nlink = 1;
    return 0;
  }

  ~Null_file() noexcept override {}
};

class Zero_file : public Devfs::Device_file
{
public:
  ssize_t readv(const struct iovec *iov, int iovcnt) noexcept override
  {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
      memset(iov[i].iov_base, 0, iov[i].iov_len);
      total += (ssize_t)iov[i].iov_len;
    }
    return total;
  }

  ssize_t writev(const struct iovec *iov, int iovcnt) noexcept override
  {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i)
      total += (ssize_t)iov[i].iov_len;
    return total;
  }

  int fstat(struct stat64 *buf) const noexcept override
  {
    memset(buf, 0, sizeof(*buf));
    buf->st_mode  = S_IFCHR | 0666;
    buf->st_rdev  = makedev(1, 5); // major=1, minor=5 (Linux /dev/zero)
    buf->st_nlink = 1;
    return 0;
  }

  ~Zero_file() noexcept override {}
};

/* ------------------------------------------------------------------ */
/* /dev directory object (mounted at the /dev mount point)             */
/* ------------------------------------------------------------------ */

class Devfs_dir : public L4Re::Vfs::Be_file
{
public:
  Devfs_dir() : _dent_idx(0) {}

  /* VFS path resolution hook.
   * Empty path → return this (opening the directory itself).
   * Non-empty path → look up device in registry. */
  int get_entry(const char *path, int /*flags*/, mode_t /*mode*/,
                cxx::Ref_ptr<L4Re::Vfs::File> *f) noexcept override
  {
    if (!path[0] || (path[0] == '.' && !path[1])) {
      *f = cxx::ref_ptr(this);
      return 0;
    }

    pthread_mutex_lock(&g_reg.lock);
    auto it = g_reg.devices.find(path);
    if (it == g_reg.devices.end()) {
      pthread_mutex_unlock(&g_reg.lock);
      return -ENOENT;
    }
    *f = it->second;
    pthread_mutex_unlock(&g_reg.lock);
    return 0;
  }

  int fstat(struct stat64 *buf) const noexcept override
  {
    memset(buf, 0, sizeof(*buf));
    buf->st_mode  = S_IFDIR | 0555;
    buf->st_ino   = 1;
    buf->st_nlink = 2;
    return 0;
  }

  int faccessat(const char *path, int /*mode*/, int /*flags*/) noexcept override
  {
    if (!path[0] || (path[0] == '.' && !path[1]))
      return 0;
    pthread_mutex_lock(&g_reg.lock);
    bool found = g_reg.devices.count(path) > 0;
    pthread_mutex_unlock(&g_reg.lock);
    return found ? 0 : -ENOENT;
  }

  int get_status_flags() const noexcept override { return O_RDONLY; }

  /* Fill readdir buffer with dirent64 entries.
   * Pattern follows ns_fs_impl.h: cursor resets when empty return. */
  ssize_t getdents(char *buf, size_t dest_sz) noexcept override
  {
    /* Snapshot device names at the start of a new scan. */
    if (_dent_idx == 0) {
      pthread_mutex_lock(&g_reg.lock);
      _dent_snap.clear();
      for (auto &e : g_reg.devices)
        _dent_snap.push_back(e.first);
      pthread_mutex_unlock(&g_reg.lock);
    }

    dirent64 *dest = reinterpret_cast<dirent64 *>(buf);
    ssize_t ret = 0;
    const size_t total = 2 + _dent_snap.size(); // ".", "..", devices

    while (_dent_idx < (int)total && dest_sz > 0) {
      const char   *name;
      unsigned char dtype;

      if      (_dent_idx == 0) { name = ".";  dtype = DT_DIR; }
      else if (_dent_idx == 1) { name = ".."; dtype = DT_DIR; }
      else                     { name = _dent_snap[_dent_idx - 2].c_str(); dtype = DT_CHR; }

      unsigned l = (unsigned)strlen(name) + 1;
      unsigned n = (unsigned)offsetof(dirent64, d_name) + l;
      n = (n + (unsigned)sizeof(long) - 1) & ~((unsigned)sizeof(long) - 1);

      if (n > dest_sz)
        break;

      dest->d_ino    = (ino64_t)(_dent_idx + 1);
      dest->d_off    = (off64_t)(_dent_idx + 1);
      dest->d_reclen = (unsigned short)n;
      dest->d_type   = dtype;
      memcpy(dest->d_name, name, l);

      ret      += n;
      dest_sz  -= n;
      dest      = reinterpret_cast<dirent64 *>(
                    reinterpret_cast<char *>(dest) + n);
      ++_dent_idx;
    }

    if (!ret)
      _dent_idx = 0; // reset for next opendir on same object (ns_fs pattern)

    return ret;
  }

  ~Devfs_dir() noexcept override {}

private:
  int                      _dent_idx;
  std::vector<std::string> _dent_snap;
};

static cxx::Ref_ptr<Devfs_dir> g_dir;

} // anonymous namespace

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

namespace Devfs {

int init()
{
  g_dir = cxx::make_ref_obj<Devfs_dir>();

  /* Built-in nodes */
  register_device("null", cxx::make_ref_obj<Null_file>());
  register_device("zero", cxx::make_ref_obj<Zero_file>());

  return L4Re::Vfs::vfs_ops->mount("dev", g_dir);
}

int register_device(const char *name, cxx::Ref_ptr<Device_file> file)
{
  if (!name || !file)
    return -EINVAL;

  pthread_mutex_lock(&g_reg.lock);
  g_reg.devices[name] = file;
  pthread_mutex_unlock(&g_reg.lock);
  return 0;
}

int unregister_device(const char *name)
{
  if (!name)
    return -EINVAL;

  pthread_mutex_lock(&g_reg.lock);
  size_t removed = g_reg.devices.erase(name);
  pthread_mutex_unlock(&g_reg.lock);
  return removed ? 0 : -ENOENT;
}

} // namespace Devfs
