/*
 * TuringOS ext4fs — client-side VFS file wrapper (streaming I/O)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Ext4_file_vfs is a Be_file_pos that wraps an Ext4_file_ops capability.
 * The constructor calls op_setup() to obtain a fixed-size shared "bounce
 * buffer" Dataspace and the current file size.  POSIX reads/writes are served
 * by chunked op_pread/op_pwrite RPCs through that buffer (write-through), so
 * there is no whole-file buffering, no 4 MiB cap, and no last-close-wins
 * lost-update bug.  op_release frees the server object on destruction.
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
  off64_t size() const noexcept override { return (off64_t)_size; }
  ssize_t preadv(const struct iovec *iov, int cnt, off64_t offset) noexcept override;
  ssize_t pwritev(const struct iovec *iov, int cnt, off64_t offset) noexcept override;
  int     fstat(struct stat64 *buf) const noexcept override;
  int     ftruncate(off64_t pos) noexcept override;

  // Writes are write-through, so there is nothing to flush on fsync/fclose.
  int     fsync() const noexcept override { return 0; }
  int     unlock_all_locks() noexcept override { return 0; }

  int     get_status_flags() const noexcept override;
  int     set_status_flags(long flags) noexcept override;

private:
  L4::Cap<Ext4_file_ops>      _cap;
  L4Re::Util::Unique_cap<L4Re::Dataspace> _buf;  // shared bounce buffer
  l4_addr_t    _buf_addr = 0;
  l4_uint32_t  _buf_size = 0;     // usable bounce-buffer capacity
  l4_uint64_t  _size     = 0;     // tracked file size (setup + writes/truncate)
  bool         _append   = false; // O_APPEND: writes go to current EOF
  bool         _valid    = false;
};
