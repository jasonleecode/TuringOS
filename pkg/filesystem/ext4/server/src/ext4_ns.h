/*
 * ext4fs Namespace server for TuringOS – Phase 4
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Implements L4Re::Namespace so that other tasks can open ext4 files
 * via standard POSIX fopen()/fread()/fwrite() through the L4Re VFS.
 *
 * Phase 3b: op_query returned a read-only Dataspace cap.
 * Phase 4:  op_query returns an Ext4_file_ops cap (per-file server object
 *           backed by a shared R/W Dataspace) so that fwrite() works too.
 */
#pragma once

#include <l4/sys/cxx/ipc_epiface>
#include <l4/re/namespace>
#include <string.h>
#include <l4/sys/cxx/ipc_array>
#include <l4/sys/cxx/ipc_types>
#include <l4/re/util/object_registry>

class Ext4_namespace
: public L4::Epiface_t<Ext4_namespace, L4Re::Namespace>
{
public:
  using Name_buffer = L4::Ipc::Array_in_buf<char, unsigned long>;

  // `prefix` is the absolute lwext4 path this namespace is rooted at, e.g.
  // "/" for the root namespace, "/subdir" for a child directory namespace.
  explicit Ext4_namespace(L4Re::Util::Object_registry *registry,
                          const char *prefix = "/")
  : _registry(registry)
  {
    size_t n = strlen(prefix);
    if (n >= sizeof(_prefix)) n = sizeof(_prefix) - 1;
    memcpy(_prefix, prefix, n);
    _prefix[n] = '\0';
  }

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

private:
  L4Re::Util::Object_registry *_registry;
  char _prefix[256];
};
