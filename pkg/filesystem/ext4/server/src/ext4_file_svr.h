/*
 * TuringOS ext4fs — per-file IPC server object (Phase 4)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */
#pragma once

#include <l4/sys/cxx/ipc_epiface>
#include <l4/re/util/unique_cap>
#include <l4/re/dataspace>
#include "../../include/ext4_file_proto.h"

// One Ext4_file_svr is created per op_query call in the Namespace server.
// It allocates a shared R/W Dataspace for the file content.
// On op_close the DS content is written back to the ext4 filesystem.
class Ext4_file_svr
: public L4::Epiface_t<Ext4_file_svr, Ext4_file_ops>
{
public:
  // Allocate DS and (if the file exists) fill it from ext4.
  // path must be an absolute lwext4 path, e.g. "/hello.txt".
  explicit Ext4_file_svr(const char *path);

  ~Ext4_file_svr();

  l4_ret_t op_get_ds(Ext4_file_ops::Rights,
                     L4::Ipc::Snd_fpage &snd_ds,
                     l4_uint64_t &size);

  l4_ret_t op_close(Ext4_file_ops::Rights, l4_uint64_t written);

private:
  char     _path[258];
  L4Re::Util::Unique_cap<L4Re::Dataspace> _ds;
  l4_addr_t    _ds_addr = 0;
  unsigned long _ds_size = 0;
  l4_uint64_t  _fsize   = 0;   // original file size (0 = not found)
  bool         _ok      = false;
};
