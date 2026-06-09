/*
 * TuringOS procfs — synthetic read-only /proc and /sys
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * This microkernel has no monolithic kernel, so /proc and /sys are not kernel
 * exports — they are an in-process synthetic VFS whose file content is
 * *generated on open* by a per-node function (a snapshot of system state).
 * Structure mirrors tmpfs (Be_file dir + Be_file_pos file); content is
 * read-only.  Linked into apps that want it; a static initialiser mounts
 * /proc and /sys after VFS init.
 *
 *   Gen_file : Be_file_pos  — snapshots its generator's text at open time
 *   Gen_dir  : Be_file      — a fixed read-only table of {name, generator}
 *
 * /sys here is a minimal placeholder: the Linux device-model hierarchy does
 * not map onto this system, so it exposes only a couple of derivable nodes.
 */

#include <l4/l4re_vfs/backend>
#include <l4/cxx/ref_ptr>
#include <l4/crtn/initpriorities.h>
#include <l4/re/env>
#include <l4/re/mem_alloc>
#include <l4/sys/scheduler>
#include <spawn_ipc.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <new>

namespace {

using L4Re::Vfs::File;
using L4Re::Vfs::Be_file;
using L4Re::Vfs::Be_file_pos;
using cxx::Ref_ptr;

#if defined(__aarch64__)
static const char *const ARCH = "aarch64";
#elif defined(__arm__)
static const char *const ARCH = "armv7";
#else
static const char *const ARCH = "unknown";
#endif

// ---------------------------------------------------------------------------
// Data sources
// ---------------------------------------------------------------------------
static unsigned cpu_count()
{
  l4_umword_t cpu_max = 0;
  l4_sched_cpu_set_t cs = l4_sched_cpu_set(0, 0, ~0UL);
  long r = l4_error(L4Re::Env::env()->scheduler()->info(&cpu_max, &cs));
  return (!r) ? (unsigned)__builtin_popcountl(cs.map) : 1;
}

// ---------------------------------------------------------------------------
// Generators — write text into buf (capacity n), return byte count (>=0).
// ---------------------------------------------------------------------------
static int gen_uptime(char *buf, int n)
{
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long sec = (long)ts.tv_sec;
  long cs  = (long)(ts.tv_nsec / 10000000);   // centiseconds, no float
  return snprintf(buf, n, "%ld.%02ld 0.00\n", sec, cs);
}

static int gen_meminfo(char *buf, int n)
{
  L4Re::Mem_alloc::Stats st{};
  unsigned long total_kb = 0, free_kb = 0;
  if (L4Re::Env::env()->mem_alloc()->info(st) == 0) {
    total_kb = (unsigned long)(st.mem_limit / 1024);
    free_kb  = (unsigned long)(st.mem_free  / 1024);
  }
  return snprintf(buf, n,
                  "MemTotal:    %8lu kB\n"
                  "MemFree:     %8lu kB\n",
                  total_kb, free_kb);
}

static int gen_version(char *buf, int n)
{
  return snprintf(buf, n, "TuringOS version Fiasco.OC/L4Re (%s)\n", ARCH);
}

static int gen_cpuinfo(char *buf, int n)
{
  unsigned ncpu = cpu_count();
  int w = 0;
  for (unsigned i = 0; i < ncpu && w < n; ++i)
    w += snprintf(buf + w, n - w,
                  "processor\t: %u\n"
                  "architecture\t: %s\n\n",
                  i, ARCH);
  return w;
}

static int gen_cpu_online(char *buf, int n)
{
  unsigned ncpu = cpu_count();
  if (ncpu <= 1) return snprintf(buf, n, "0\n");
  return snprintf(buf, n, "0-%u\n", ncpu - 1);
}

static int gen_tasks(char *buf, int n)
{
  int w = snprintf(buf, n, "HANDLE  STATE    CPU_US\n");
  auto sp = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
  if (!sp.is_valid())
    return w;
  for (int slot = 0; slot < 32 && w < n; ++slot) {
    l4_uint64_t cpu_us = 0;
    l4_uint32_t state = 0, handle = 0;
    if (sp->task_stat((l4_uint32_t)slot, cpu_us, state, handle) < 0)
      continue;
    const char *s = (state == 1) ? "RUNNING" : (state == 2) ? "EXITED" : "?";
    w += snprintf(buf + w, n - w, "%6u  %-7s  %llu\n",
                  handle, s, (unsigned long long)cpu_us);
  }
  return w;
}

// ---------------------------------------------------------------------------
// Gen_file — a read-only file holding a one-shot snapshot of its generator.
// ---------------------------------------------------------------------------
typedef int (*gen_fn)(char *buf, int bufsz);

class Gen_file : public Be_file_pos
{
public:
  explicit Gen_file(gen_fn gen) noexcept : Be_file_pos()
  {
    _buf = static_cast<char *>(malloc(BUFSZ));
    if (_buf) {
      int w = gen(_buf, BUFSZ);
      if (w < 0)            w = 0;
      else if (w >= BUFSZ)  w = BUFSZ - 1;   // truncated
      _size = (size_t)w;
    }
  }
  ~Gen_file() noexcept { free(_buf); }

  off64_t size() const noexcept override { return (off64_t)_size; }

  ssize_t preadv(const struct iovec *iov, int cnt, off64_t off) noexcept override
  {
    if (!_buf) return -EIO;
    ssize_t total = 0;
    for (int i = 0; i < cnt; ++i) {
      if (off < 0 || (size_t)off >= _size) break;
      size_t avail = _size - (size_t)off;
      size_t todo  = iov[i].iov_len;
      if (todo > avail) todo = avail;
      if (!todo) break;
      memcpy(iov[i].iov_base, _buf + off, todo);
      off   += (off64_t)todo;
      total += (ssize_t)todo;
    }
    return total;
  }

  // Read-only synthetic file.
  ssize_t pwritev(const struct iovec *, int, off64_t) noexcept override
  { return -EROFS; }

  int fstat(struct stat64 *b) const noexcept override
  {
    memset(b, 0, sizeof(*b));
    b->st_mode    = S_IFREG | 0444;
    b->st_size    = (off64_t)_size;
    b->st_blksize = 4096;
    return 0;
  }

  int get_status_flags() const noexcept override { return O_RDONLY; }

private:
  static const int BUFSZ = 8192;
  char  *_buf  = nullptr;
  size_t _size = 0;
};

// ---------------------------------------------------------------------------
// Gen_dir — a flat, read-only directory of generated files.
// ---------------------------------------------------------------------------
struct Gen_node { const char *name; gen_fn gen; };

class Gen_dir : public Be_file
{
public:
  Gen_dir(Gen_node const *nodes, unsigned count) noexcept
  : _nodes(nodes), _count(count) {}

  int get_entry(const char *path, int flags, mode_t,
                Ref_ptr<File> *f) noexcept override
  {
    while (*path == '/') ++path;
    if (!*path) { *f = cxx::ref_ptr(this); return 0; }   // the dir itself
    if (strchr(path, '/')) return -ENOENT;               // flat: no nesting

    for (unsigned i = 0; i < _count; ++i)
      if (strcmp(path, _nodes[i].name) == 0) {
        if (flags & (O_WRONLY | O_RDWR)) return -EACCES;  // read-only fs
        *f = cxx::make_ref_obj<Gen_file>(_nodes[i].gen);
        return *f ? 0 : -ENOMEM;
      }
    return -ENOENT;
  }

  int faccessat(const char *path, int mode, int) noexcept override
  {
    while (*path == '/') ++path;
    if (!*path) return 0;
    if (mode & W_OK) return -EACCES;
    for (unsigned i = 0; i < _count; ++i)
      if (strcmp(path, _nodes[i].name) == 0) return 0;
    return -ENOENT;
  }

  int fstat(struct stat64 *b) const noexcept override
  {
    memset(b, 0, sizeof(*b));
    b->st_mode    = S_IFDIR | 0555;
    b->st_nlink   = 2;
    b->st_blksize = 4096;
    return 0;
  }

  ssize_t getdents(char *buf, size_t sz) noexcept override
  {
    struct dirent64 *d = reinterpret_cast<struct dirent64 *>(buf);
    ssize_t ret = 0;
    while (_dent_pos < _count) {
      const char *name = _nodes[_dent_pos].name;
      size_t nl = strlen(name) + 1;
      if (nl > sizeof(d->d_name)) nl = sizeof(d->d_name);
      unsigned reclen = offsetof(struct dirent64, d_name) + nl;
      reclen = (reclen + sizeof(long) - 1) & ~(sizeof(long) - 1);
      if (reclen > sz) break;

      d->d_ino    = 1;
      d->d_off    = 0;
      d->d_reclen = (unsigned short)reclen;
      d->d_type   = DT_REG;
      memcpy(d->d_name, name, nl - 1);
      d->d_name[nl - 1] = 0;

      ret += reclen;
      sz  -= reclen;
      d = reinterpret_cast<struct dirent64 *>(
            reinterpret_cast<char *>(d) + reclen);
      ++_dent_pos;
    }
    if (!ret) _dent_pos = 0;   // end of stream; reset for next opendir
    return ret;
  }

private:
  Gen_node const *_nodes;
  unsigned        _count;
  unsigned        _dent_pos = 0;
};

// ---------------------------------------------------------------------------
// Mount /proc and /sys once, after VFS init.
// ---------------------------------------------------------------------------
static const Gen_node proc_nodes[] = {
  { "uptime",  gen_uptime  },
  { "meminfo", gen_meminfo },
  { "version", gen_version },
  { "cpuinfo", gen_cpuinfo },
  { "tasks",   gen_tasks   },
};

static const Gen_node sys_nodes[] = {
  { "cpu_online", gen_cpu_online },
  { "version",    gen_version    },
};

struct Procfs_init
{
  Procfs_init()
  {
    if (!L4Re::Vfs::vfs_ops) return;
    // No leading slash (see tmpfs): "proc" / "sys", not "/proc".
    Ref_ptr<File> p = cxx::make_ref_obj<Gen_dir>(
        proc_nodes, sizeof(proc_nodes) / sizeof(proc_nodes[0]));
    if (p) L4Re::Vfs::vfs_ops->mount("proc", p);
    Ref_ptr<File> s = cxx::make_ref_obj<Gen_dir>(
        sys_nodes, sizeof(sys_nodes) / sizeof(sys_nodes[0]));
    if (s) L4Re::Vfs::vfs_ops->mount("sys", s);
  }
} _procfs_init __attribute__((init_priority(INIT_PRIO_LATE + 1)));

} // namespace

// Referenced via `-u procfs_module_init` so this TU (and its static
// initialiser) is pulled in from the static library.
extern "C" void procfs_module_init() {}
