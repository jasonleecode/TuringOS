/*
 * TuringOS ext4fs — client-side VFS file wrapper (Phase 4)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Ext4_file_vfs is a Be_file_pos that wraps an Ext4_file_ops capability.
 * The constructor calls op_get_ds() to obtain a shared R/W Dataspace
 * pre-filled with the existing file content.  POSIX reads/writes go to
 * the mapped DS memory.  When fclose() triggers unlock_all_locks(), the
 * dirty bytes are flushed to disk via op_close().
 *
 * Ext4_file_factory is a File_factory_t registered at startup so that
 * cap_to_vfs_object() can create Ext4_file_vfs from Ext4_file_ops caps.
 */
#pragma once

#include <l4/l4re_vfs/backend>
#include <l4/re/util/unique_cap>
#include <l4/re/dataspace>
#include "../../include/ext4_file_proto.h"

class Ext4_file_vfs : public L4Re::Vfs::Be_file_pos
{
public:
  explicit Ext4_file_vfs(L4::Cap<Ext4_file_ops> cap) noexcept;
  ~Ext4_file_vfs() noexcept;

  // Be_file_pos interface
  off64_t size() const noexcept override { return (off64_t)_visible_size(); }
  ssize_t preadv(const struct iovec *iov, int cnt, off64_t offset) noexcept override;
  ssize_t pwritev(const struct iovec *iov, int cnt, off64_t offset) noexcept override;
  int     fstat(struct stat64 *buf) const noexcept override;
  int     ftruncate(off64_t pos) noexcept override;
  int     fsync() const noexcept override;
  int     get_status_flags() const noexcept override;

  // Called by fclose() — flushes dirty data.
  int unlock_all_locks() noexcept override;

private:
  l4_uint64_t _visible_size() const noexcept
  { return (_written > _fsize) ? _written : _fsize; }

  L4::Cap<Ext4_file_ops>      _cap;
  L4Re::Util::Unique_cap<L4Re::Dataspace> _ds;
  l4_addr_t   _ds_addr  = 0;
  unsigned long _ds_size = 0;
  l4_uint64_t  _fsize   = 0;  // size at open time
  l4_uint64_t  _written = 0;  // highest write offset reached
  bool         _dirty   = false; // true if any write or truncate occurred
  bool         _valid   = false;
};
