/*
 * ext4fs Namespace server – op_query implementation (Phase 4)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Phase 4 change: instead of allocating a Dataspace here and returning it
 * as a read-only cap, we create a per-file Ext4_file_svr IPC object and
 * return its capability.  The client's File_factory for Ext4_file_ops
 * (registered by the ext4_client library) wraps it in an Ext4_file_vfs
 * that supports both fread() and fwrite().
 */

#include "ext4_ns.h"
#include "ext4_file_svr.h"

#include <string.h>
#include <stdio.h>
#include <new>

l4_ret_t
Ext4_namespace::op_query(L4Re::Namespace::Rights,
                         Name_buffer const &name,
                         L4::Ipc::Snd_fpage &snd_cap,
                         L4::Ipc::Opt<L4::Opcode> &dummy,
                         L4::Ipc::Opt<L4::Ipc::Array_ref<char, unsigned long>> &out_name)
{
  // Resolve the first path component (stop at '/').
  auto const *sep =
    static_cast<char const *>(memchr(name.data, '/', name.length));
  unsigned long part = sep ? (unsigned long)(sep - name.data) : name.length;

  if (part == 0 || part > 255)
    return -L4_EINVAL;

  // Build the absolute lwext4 path, e.g. "/hello.txt".
  char path[258];
  path[0] = '/';
  memcpy(path + 1, name.data, part);
  path[part + 1] = '\0';

  // Allocate a per-file server object (heap, stays alive for server lifetime).
  auto *file_svr = new(std::nothrow) Ext4_file_svr(path);
  if (!file_svr)
    return -L4_ENOMEM;

  // Register the file server in the IPC registry → allocates an IPC gate.
  L4::Cap<void> svr_cap = _registry->register_obj(file_svr);
  if (!svr_cap.is_valid())
    {
      delete file_svr;
      return -L4_ENOMEM;
    }

  // Send the IPC gate cap with full rights.
  // The client's File_factory<Ext4_file_ops, Ext4_file_vfs> matches the
  // protocol (0x5800) and creates the appropriate VFS file object.
  snd_cap = L4::Ipc::Snd_fpage(svr_cap, L4_CAP_FPAGE_RWS);

  dummy.set_valid();
  out_name.set_valid();

  if (sep)
    {
      // Partial resolution — return the remaining sub-path.
      unsigned long rem = name.length - part - 1;
      memcpy(out_name->data, name.data + part + 1, rem);
      out_name->length = rem;
      return L4Re::Namespace::Partly_resolved;
    }

  out_name->length = 0;
  return L4_EOK;
}
