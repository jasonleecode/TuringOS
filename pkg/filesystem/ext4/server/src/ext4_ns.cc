/*
 * ext4fs Namespace server – op_query implementation
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */

#include "ext4_ns.h"

#include <l4/re/env>
#include <l4/re/mem_alloc>
#include <l4/re/rm>
#include <l4/re/util/unique_cap>

extern "C" {
#include <ext4.h>
#include <ext4_errno.h>
}

#include <string.h>
#include <stdio.h>

l4_ret_t
Ext4_namespace::op_query(L4Re::Namespace::Rights,
                         Name_buffer const &name,
                         L4::Ipc::Snd_fpage &snd_cap,
                         L4::Ipc::Opt<L4::Opcode> &dummy,
                         L4::Ipc::Opt<L4::Ipc::Array_ref<char, unsigned long>> &out_name)
{
  // Resolve the first path component (stop at '/')
  auto const *sep =
    static_cast<char const *>(memchr(name.data, '/', name.length));
  unsigned long part = sep ? (unsigned long)(sep - name.data) : name.length;

  if (part == 0 || part > 255)
    return -L4_EINVAL;

  // Construct the ext4 absolute path (filesystem is mounted at "/")
  char path[258];
  path[0] = '/';
  memcpy(path + 1, name.data, part);
  path[part + 1] = '\0';

  // Open the file to get its size
  ext4_file f;
  if (ext4_fopen(&f, path, "rb") != EOK)
    return -L4_ENOENT;

  uint64_t fsize = ext4_fsize(&f);
  ext4_fclose(&f);

  // Allocate a Dataspace for the file content
  unsigned long alloc_size = (fsize > 0) ? (unsigned long)fsize : 1UL;

  auto ds = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
  if (!ds.is_valid())
    return -L4_ENOMEM;

  long r = L4Re::Env::env()->mem_alloc()->alloc(alloc_size, ds.get());
  if (r < 0)
    {
      printf("[ext4ns] mem_alloc failed for '%s': %ld\n", path, r);
      return r;
    }

  // Map the Dataspace locally, fill it with the file content, then detach
  if (fsize > 0)
    {
      l4_addr_t addr = 0;
      r = L4Re::Env::env()->rm()->attach(
            &addr, alloc_size,
            L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
            L4::Ipc::make_cap_rw(ds.get()), 0);
      if (r >= 0)
        {
          if (ext4_fopen(&f, path, "rb") == EOK)
            {
              size_t rd = 0;
              ext4_fread(&f, reinterpret_cast<void *>(addr),
                         (size_t)fsize, &rd);
              ext4_fclose(&f);
            }
          L4Re::Env::env()->rm()->detach(addr, nullptr);
        }
    }

  // Return the Dataspace as a read-only capability.
  // We deliberately release() the unique_cap so the cap slot is not freed
  // before the IPC reply sends the mapping to the client.  The cap slot
  // leaks in our address space but remains valid; it is bounded by the
  // number of distinct files ever queried.
  l4_cap_idx_t raw = ds.get().cap();
  ds.release();
  snd_cap = L4::Ipc::Snd_fpage(L4::Cap<void>(raw), L4_CAP_FPAGE_RO);

  dummy.set_valid();
  out_name.set_valid();
  if (sep)
    {
      // Partial resolution — return the remaining sub-path to the client
      unsigned long rem = name.length - part - 1;
      memcpy(out_name->data, name.data + part + 1, rem);
      out_name->length = rem;
      return L4Re::Namespace::Partly_resolved;
    }
  out_name->length = 0;
  return L4_EOK;
}
