/*
 * TuringOS ext4fs — per-file IPC protocol (streaming I/O)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Two custom L4 protocols outside L4Re's 0x4000..0x401x range:
 *   Ext4_file_ops  (0x5800) — per-open-file: streaming offset read/write
 *                              through a fixed shared bounce buffer.
 *
 * Design: the server keeps the underlying ext4 file open and seeks+reads/
 * writes at explicit offsets, transferring data through one fixed-size R/W
 * "bounce buffer" Dataspace shared with the client.  This replaces the old
 * whole-file-into-a-DS model, removing the 4 MiB cap, the per-open whole-file
 * memory cost, and the last-close-wins lost-update bug (writes are write-
 * through; synchronous RPCs serialise buffer access on the single-threaded
 * server).
 */
#pragma once

#include <l4/sys/cxx/ipc_iface>
#include <l4/sys/cxx/ipc_types>
#include <l4/re/dataspace>
#include <l4/re/util/unique_cap>
#include <l4/re/cap_alloc>

// Per-file handle protocol.  One object per open file, allocated by the
// ext4fs server when a client calls op_query on the Namespace server.
struct Ext4_file_ops
: L4::Kobject_t<Ext4_file_ops, L4::Kobject, 0x5800>
{
  // Open the underlying ext4 file (creating it if absent) and hand back the
  // shared bounce-buffer Dataspace plus the current file size.  Called once
  // by the client right after it receives this cap.  `buf_size` is the usable
  // byte capacity of the bounce buffer (max len for one pread/pwrite).
  L4_INLINE_RPC_NF(long, setup,
    (L4::Ipc::Small_buf recv_buf,      // client-side: cap slot for buffer DS
     L4::Ipc::Snd_fpage &snd_buf,      // server-side: buffer DS cap to send
     l4_uint64_t &size,                // out: current file size
     l4_uint32_t &buf_size));          // out: bounce buffer capacity

  // Read up to `len` bytes at file offset `off` into the bounce buffer.
  // Returns L4_EOK with `got` = bytes read (0 at EOF).  len is clamped to the
  // buffer capacity by the server.
  L4_INLINE_RPC(long, pread,
    (l4_uint64_t off, l4_uint32_t len, l4_uint32_t &got));

  // Write `len` bytes from the bounce buffer at file offset `off` (write-
  // through to disk).  Returns L4_EOK with `put` = bytes written.
  L4_INLINE_RPC(long, pwrite,
    (l4_uint64_t off, l4_uint32_t len, l4_uint32_t &put));

  // Atomic append (O_APPEND): seek to the current end of file and write `len`
  // bytes from the bounce buffer there.  Returns `put` = bytes written and
  // `off` = the offset the data landed at (EOF at write time).  The single-
  // threaded server serialises this, so concurrent appenders never overwrite
  // each other — unlike a client computing the offset from a cached size.
  L4_INLINE_RPC(long, pappend,
    (l4_uint32_t len, l4_uint32_t &put, l4_uint64_t &off));

  // Truncate the file to `size` bytes.
  L4_INLINE_RPC(long, ftruncate, (l4_uint64_t size));

  // Release the per-open server object: the client is done with this file
  // handle (last fd closed).  The server closes the ext4 file, frees the
  // bounce buffer + IPC gate, and self-destructs.  Called unconditionally
  // from the client's destructor so read-only opens are reclaimed too.
  // After release the cap is dead; do not reuse it.
  L4_INLINE_RPC(long, release, (void));

  // Convenience wrapper: call setup and receive the buffer DS into `buf_slot`.
  long setup(L4::Cap<L4Re::Dataspace> buf_slot, l4_uint64_t &size,
             l4_uint32_t &buf_size) const noexcept
  {
    L4::Ipc::Snd_fpage snd;
    return setup_t::call(c(), L4::Ipc::Small_buf(buf_slot), snd, size, buf_size);
  }

  // Opcode order = list order.  Keep pappend_t LAST so adding it doesn't shift
  // the opcodes of existing RPCs (spawnd uses release_t and need not rebuild
  // in lockstep).
  typedef L4::Typeid::Rpcs<setup_t, pread_t, pwrite_t,
                           ftruncate_t, release_t, pappend_t> Rpcs;
};

// Directory-mutation protocol on the root Ext4_namespace cap.
// Operations take absolute ext4 paths (e.g. "/foo/bar").
struct Ext4_dir_ops
: L4::Kobject_t<Ext4_dir_ops, L4::Kobject, 0x5801>
{
  L4_INLINE_RPC(long, ext_mkdir,
    (L4::Ipc::Array<char const, unsigned long> path));
  L4_INLINE_RPC(long, ext_unlink,
    (L4::Ipc::Array<char const, unsigned long> path));

  typedef L4::Typeid::Rpcs<ext_mkdir_t, ext_unlink_t> Rpcs;
};
