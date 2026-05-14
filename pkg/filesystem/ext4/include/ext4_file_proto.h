/*
 * TuringOS ext4fs — per-file IPC protocol (Phase 4: write support)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Two custom L4 protocols outside L4Re's 0x4000..0x401x range:
 *   Ext4_file_ops  (0x5800) — per-open-file: get shared DS, commit on close
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
  // Return a shared R/W Dataspace containing the file content, plus file
  // size.  The Dataspace is pre-filled from disk.  The client maps it
  // for local reads/writes.
  L4_INLINE_RPC_NF(long, get_ds,
    (L4::Ipc::Small_buf recv_ds,       // client-side: cap slot for DS
     L4::Ipc::Snd_fpage &snd_ds,       // server-side: DS cap to send
     l4_uint64_t &size));              // out: original file size

  // Flush first `written` bytes of the shared DS back to disk and close.
  // Pass written=0 to skip flush (read-only access).
  L4_INLINE_RPC(long, close, (l4_uint64_t written));

  // Convenience wrapper: call get_ds and receive DS into `ds_slot`.
  long get_ds(L4::Cap<L4Re::Dataspace> ds_slot, l4_uint64_t &size) const noexcept
  {
    L4::Ipc::Snd_fpage snd;
    return get_ds_t::call(c(), L4::Ipc::Small_buf(ds_slot), snd, size);
  }

  typedef L4::Typeid::Rpcs<get_ds_t, close_t> Rpcs;
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
