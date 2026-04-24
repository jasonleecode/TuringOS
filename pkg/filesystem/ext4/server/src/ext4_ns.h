/*
 * ext4fs Namespace server for TuringOS – Phase 3b
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Implements L4Re::Namespace so that other tasks can open ext4 files
 * via standard POSIX fopen()/fread() through the L4Re in-process VFS.
 *
 * Each op_query() call opens the requested file from the mounted ext4
 * filesystem, reads its contents into a newly allocated L4Re::Dataspace,
 * and returns the Dataspace capability.  Clients receive it as a
 * read-only memory-mapped file via the VFS File_factory for Dataspaces.
 */

#pragma once

#include <l4/sys/cxx/ipc_epiface>
#include <l4/re/namespace>
#include <l4/sys/cxx/ipc_array>
#include <l4/sys/cxx/ipc_types>

class Ext4_namespace
: public L4::Epiface_t<Ext4_namespace, L4Re::Namespace>
{
public:
  using Name_buffer = L4::Ipc::Array_in_buf<char, unsigned long>;

  l4_ret_t op_query(L4Re::Namespace::Rights,
                    Name_buffer const &name,
                    L4::Ipc::Snd_fpage &snd_cap,
                    L4::Ipc::Opt<L4::Opcode> &dummy,
                    L4::Ipc::Opt<L4::Ipc::Array_ref<char, unsigned long>> &out_name);

  l4_ret_t op_register_obj(L4Re::Namespace::Rights, unsigned,
                            Name_buffer const &,
                            L4::Ipc::Snd_fpage &)
  { return -L4_EPERM; }

  l4_ret_t op_unlink(L4Re::Namespace::Rights, Name_buffer const &)
  { return -L4_EPERM; }
};
