/*
 * TuringOS ext4fs — per-file IPC server object (streaming I/O)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */
#pragma once

#include <l4/sys/cxx/ipc_epiface>
#include <l4/re/util/unique_cap>
#include <l4/re/util/object_registry>
#include <l4/re/dataspace>
#include "../../include/ext4_file_proto.h"

extern "C" {
#include <ext4.h>
}

// One Ext4_file_svr is created per op_query call in the Namespace server.
// It keeps the underlying ext4 file open and serves streaming offset
// read/write through a fixed-size shared "bounce buffer" Dataspace, so there
// is no whole-file buffering (no 4 MiB cap, bounded memory, write-through).
// op_release closes the file, frees the buffer + IPC gate, and self-destructs.
class Ext4_file_svr
: public L4::Epiface_t<Ext4_file_svr, Ext4_file_ops>
{
public:
  // Allocate the bounce buffer.  The underlying ext4 file is opened lazily in
  // op_setup (so creation/flags are handled at open time).
  // path must be an absolute lwext4 path, e.g. "/hello.txt".
  // `registry` is used by op_release to unregister this object.
  Ext4_file_svr(L4Re::Util::Object_registry *registry, const char *path);

  ~Ext4_file_svr();

  l4_ret_t op_setup(Ext4_file_ops::Rights,
                    L4::Ipc::Snd_fpage &snd_buf,
                    l4_uint64_t &size,
                    l4_uint32_t &buf_size);

  l4_ret_t op_pread(Ext4_file_ops::Rights,
                    l4_uint64_t off, l4_uint32_t len, l4_uint32_t &got);

  l4_ret_t op_pwrite(Ext4_file_ops::Rights,
                     l4_uint64_t off, l4_uint32_t len, l4_uint32_t &put);

  l4_ret_t op_pappend(Ext4_file_ops::Rights,
                      l4_uint32_t len, l4_uint32_t &put, l4_uint64_t &off);

  l4_ret_t op_ftruncate(Ext4_file_ops::Rights, l4_uint64_t size);

  l4_ret_t op_release(Ext4_file_ops::Rights);

private:
  // Bounce buffer capacity (bytes).  One pread/pwrite transfers at most this
  // much; the client loops for larger I/O.  64 KiB balances RPC count vs the
  // per-open memory footprint (this much in the server + the same mapped in
  // the client).
  static const unsigned BUF_SIZE = 64u * 1024u;

  bool ensure_open();   // open _f on first use; returns _f_open

  L4Re::Util::Object_registry *_registry;
  char         _path[258];
  ext4_file    _f;
  bool         _f_open  = false;
  L4Re::Util::Unique_cap<L4Re::Dataspace> _buf;
  l4_addr_t    _buf_addr = 0;
  bool         _ok       = false;
};
