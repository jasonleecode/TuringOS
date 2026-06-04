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
#include <sys/stat.h>
#include <errno.h>

// --------------------------------------------------------------------------
// Ext4_file_vfs implementation
// --------------------------------------------------------------------------

Ext4_file_vfs::Ext4_file_vfs(L4::Cap<Ext4_file_ops> cap) noexcept
: Be_file_pos(), _cap(cap)
{
  // Allocate a local cap slot for the DS we'll receive from the server.
  _ds = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
  if (!_ds.is_valid())
    return;

  // Ask the server for the shared DS and file size.
  l4_uint64_t fsize = 0;
  long r = cap->get_ds(_ds.get(), fsize);
  if (r < 0)
    return;

  _fsize = fsize;

  // Calculate how large the DS actually is (at least one page).
  unsigned long raw = fsize ? (unsigned long)fsize : 1UL;
  _ds_size = (raw + 4095UL) & ~4095UL;
  if (_ds_size < 4096UL) _ds_size = 4096UL;

  // Map the DS locally for read/write.
  _ds_addr = 0;
  r = L4Re::Env::env()->rm()->attach(
        &_ds_addr, _ds_size,
        L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
        L4::Ipc::make_cap_rw(_ds.get()), 0);
  if (r < 0)
    { _ds_addr = 0; return; }

  _valid = true;
}

Ext4_file_vfs::~Ext4_file_vfs() noexcept
{
  if (_ds_addr)
    L4Re::Env::env()->rm()->detach(_ds_addr, nullptr);
  // _ds unique_cap frees the slot.
  // Note: we do NOT call op_close here — it was already called by
  // unlock_all_locks() which fclose() invokes before the destructor.

  // Tell the server this handle is gone so it can free the per-open
  // Ext4_file_svr (object + IPC gate + DS cap).  This runs for *every* open,
  // including read-only ones that never trigger op_close — without it the
  // server leaks a cap slot per open and a long-running system exhausts caps.
  // Best-effort: ignore the result, the cap is about to be dropped anyway.
  if (_cap.is_valid())
    _cap->release();
}

int Ext4_file_vfs::unlock_all_locks() noexcept
{
  if (_valid && _dirty)
    _cap->close(_written);
  return 0;
}

ssize_t Ext4_file_vfs::preadv(const struct iovec *iov, int cnt,
                               off64_t offset) noexcept
{
  if (!_valid || !_ds_addr) return -EIO;

  l4_uint64_t vis = _visible_size();
  ssize_t total = 0;

  for (int i = 0; i < cnt; ++i)
    {
      if (offset >= (off64_t)vis) break;

      size_t avail = (size_t)(vis - (l4_uint64_t)offset);
      size_t todo  = iov[i].iov_len;
      if (todo > avail) todo = avail;
      if (todo == 0)    break;

      memcpy(iov[i].iov_base,
             reinterpret_cast<char *>(_ds_addr) + offset, todo);
      offset += (off64_t)todo;
      total  += (ssize_t)todo;
    }
  return total;
}

ssize_t Ext4_file_vfs::pwritev(const struct iovec *iov, int cnt,
                                off64_t offset) noexcept
{
  if (!_valid || !_ds_addr) return -EIO;

  ssize_t total = 0;

  for (int i = 0; i < cnt; ++i)
    {
      size_t avail =
        ((l4_uint64_t)offset < (l4_uint64_t)_ds_size)
          ? (size_t)(_ds_size - (l4_uint64_t)offset)
          : 0;
      size_t todo = iov[i].iov_len;
      if (todo > avail) todo = avail;
      if (todo == 0)    break;

      memcpy(reinterpret_cast<char *>(_ds_addr) + offset,
             iov[i].iov_base, todo);
      offset += (off64_t)todo;
      total  += (ssize_t)todo;

      if ((l4_uint64_t)offset > _written)
        _written = (l4_uint64_t)offset;
      _dirty = true;
    }
  return total;
}

int Ext4_file_vfs::fstat(struct stat64 *buf) const noexcept
{
  memset(buf, 0, sizeof(*buf));
  buf->st_mode    = S_IFREG | 0644;
  buf->st_size    = (off64_t)_visible_size();
  buf->st_blksize = 4096;
  buf->st_blocks  = (buf->st_size + 511) / 512;
  return 0;
}

int Ext4_file_vfs::ftruncate(off64_t pos) noexcept
{
  if (!_valid || pos < 0) return -EINVAL;

  l4_uint64_t new_size = (l4_uint64_t)pos;

  // If truncating to smaller: zero the tail in DS; update _written.
  if (new_size < _visible_size() && _ds_addr)
    {
      l4_uint64_t from = new_size;
      l4_uint64_t to   = _visible_size();
      if (to > _ds_size) to = _ds_size;
      if (from < to)
        memset(reinterpret_cast<char *>(_ds_addr) + from, 0,
               (size_t)(to - from));
    }

  _written = new_size;
  _dirty   = true;
  return 0;
}

int Ext4_file_vfs::fsync() const noexcept
{
  if (_valid && _dirty)
    const_cast<Ext4_file_vfs *>(this)->_cap->close(_written);
  return 0;
}

int Ext4_file_vfs::get_status_flags() const noexcept
{ return O_RDWR; }

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

// Registration wrapper: runs after VFS is initialised (INIT_PRIO_VFS_INIT),
// before any library constructors that might open files.
struct Ext4_vfs_init
{
  Ext4_vfs_init()
  {
    auto ptr = cxx::ref_ptr(&ext4_file_factory);
    L4Re::Vfs::vfs_ops->register_file_factory(ptr);
    ptr.release(); // prevent deletion of the static object
  }
} _ext4_vfs_init __attribute__((init_priority(INIT_PRIO_LATE + 1)));

} // anonymous namespace
