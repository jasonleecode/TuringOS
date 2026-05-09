/*
 * ext4fs Namespace server – op_query implementation (Phase 5: ls support)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * L4Re's Ns_dir::getdents() enumerates a namespace by querying the special
 * ".dirinfo" entry.  The server must return a Dataspace whose content is a
 * sequence of lines in the form:
 *
 *   <name-length>:<name>\n
 *
 * e.g.  "8:hello.txt\n5:notes\n"
 *
 * All other names are resolved to either a directory sub-namespace (if the
 * entry is a lwext4 directory) or a per-file Ext4_file_svr IPC object (if
 * it is a regular file).  This lets both `ls` and `cat`/`echo >` work.
 */

#include "ext4_ns.h"
#include "ext4_file_svr.h"

extern "C" {
#include <ext4.h>
#include <ext4_errno.h>
}

#include <l4/re/env>
#include <l4/re/mem_alloc>
#include <l4/re/rm>
#include <l4/re/util/unique_cap>

#include <string.h>
#include <stdio.h>
#include <new>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Round `v` up to the next power-of-2 `align`.
static inline size_t round_page(size_t v)
{ return (v + 4095UL) & ~4095UL; }

// Build a ".dirinfo" Dataspace for `dir_path` (absolute lwext4 path, e.g. "/" or "/subdir").
// The DS contains lines of the form  "<name-length>:<name>\n".
// Returns L4_EOK and sets snd_cap on success.
static l4_ret_t
make_dirinfo(const char *dir_path, L4::Ipc::Snd_fpage &snd_cap)
{
  ext4_dir dir;
  if (ext4_dir_open(&dir, dir_path) != EOK)
    return -L4_ENOENT;

  // First pass: calculate required buffer size.
  size_t buf_needed = 0;
  const ext4_direntry *de;
  while ((de = ext4_dir_entry_next(&dir)) != nullptr)
    {
      unsigned n = de->name_length;
      if (n == 0) continue;
      if (n == 1 && de->name[0] == '.') continue;
      if (n == 2 && de->name[0] == '.' && de->name[1] == '.') continue;
      // "<digits>:<name><type_tag>\n" — at most 3 digits + colon + name + tag + newline
      buf_needed += 6 + n;
    }
  ext4_dir_close(&dir);

  if (buf_needed == 0)
    buf_needed = 1; // always need at least one page

  size_t ds_size = round_page(buf_needed);

  // Allocate a Dataspace for the dirinfo content.
  auto ds = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
  if (!ds.is_valid())
    return -L4_ENOMEM;

  long r = L4Re::Env::env()->mem_alloc()->alloc(ds_size, ds.get());
  if (r < 0)
    return -L4_ENOMEM;

  // Map it temporarily into the server's address space to fill.
  l4_addr_t addr = 0;
  r = L4Re::Env::env()->rm()->attach(
        &addr, ds_size,
        L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
        L4::Ipc::make_cap_rw(ds.get()), 0);
  if (r < 0)
    return -L4_ENOMEM;

  // Second pass: write the dirinfo lines.
  char *p   = reinterpret_cast<char *>(addr);
  char *end = p + ds_size;

  if (ext4_dir_open(&dir, dir_path) == EOK)
    {
      while ((de = ext4_dir_entry_next(&dir)) != nullptr)
        {
          unsigned n = de->name_length;
          if (n == 0) continue;
          if (n == 1 && de->name[0] == '.') continue;
          if (n == 2 && de->name[0] == '.' && de->name[1] == '.') continue;

          size_t remain = (size_t)(end - p);
          int hdr = snprintf(p, remain, "%u:", n);
          if (hdr < 0 || (size_t)hdr >= remain) break;
          p += hdr;

          // type tag: 'd' for directory, 'f' for regular file
          char type_tag = (de->inode_type == EXT4_DE_DIR) ? 'd' : 'f';
          if ((size_t)(end - p) < n + 2) break;
          memcpy(p, de->name, n);
          p += n;
          *p++ = type_tag;
          *p++ = '\n';
        }
      ext4_dir_close(&dir);
    }

  // Unmap from server address space; client will map it via the cap.
  L4Re::Env::env()->rm()->detach(addr, nullptr);

  // Return the Dataspace cap to the client.
  // The server's Unique_cap destructor will free the local cap slot, but the
  // Dataspace kernel object stays alive as long as the client holds its copy.
  snd_cap = L4::Ipc::Snd_fpage(L4::Cap<void>(ds.get().cap()), L4_CAP_FPAGE_RW);

  // Prevent Unique_cap from freeing the cap slot — we want the kernel object
  // to remain accessible via the server's cap until IPC returns and the cap
  // is mapped into the client.  Actually Unique_cap only frees the *slot*, not
  // the kernel object (the client holds a reference), so it is safe to let the
  // destructor run.  We explicitly release() to be clear about ownership.
  ds.release();

  return L4_EOK;
}

// ---------------------------------------------------------------------------
// op_query
// ---------------------------------------------------------------------------

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

  // ---- Special case: Ns_dir::getdents() queries ".dirinfo" ----------------
  // Return a Dataspace with "<namelen>:<name>\n" lines for each entry in
  // this namespace's directory (_prefix).
  if (part == 8 && memcmp(name.data, ".dirinfo", 8) == 0)
    {
      dummy.set_valid();
      out_name.set_valid();
      out_name->length = 0;
      return make_dirinfo(_prefix, snd_cap);
    }

  // ---- Build the absolute lwext4 path: _prefix + "/" + component ----------
  char path[512];
  size_t plen = strlen(_prefix);
  // _prefix is either "/" or "/subdir" — avoid double slash at root.
  if (plen == 1 && _prefix[0] == '/')
    {
      path[0] = '/';
      memcpy(path + 1, name.data, part);
      path[part + 1] = '\0';
    }
  else
    {
      memcpy(path, _prefix, plen);
      path[plen] = '/';
      memcpy(path + plen + 1, name.data, part);
      path[plen + 1 + part] = '\0';
    }

  // ---- Check if path is a directory ---------------------------------------
  // Return a child Ext4_namespace rooted at `path` so opendir() and ls work.
  ext4_dir probe;
  if (ext4_dir_open(&probe, path) == EOK)
    {
      ext4_dir_close(&probe);

      auto *sub_ns = new(std::nothrow) Ext4_namespace(_registry, path);
      if (!sub_ns)
        return -L4_ENOMEM;

      L4::Cap<void> sub_cap = _registry->register_obj(sub_ns);
      if (!sub_cap.is_valid())
        {
          delete sub_ns;
          return -L4_ENOMEM;
        }

      snd_cap = L4::Ipc::Snd_fpage(sub_cap, L4_CAP_FPAGE_RWS);
      dummy.set_valid();
      out_name.set_valid();

      if (sep)
        {
          unsigned long rem = name.length - part - 1;
          memcpy(out_name->data, name.data + part + 1, rem);
          out_name->length = rem;
          return L4Re::Namespace::Partly_resolved;
        }

      out_name->length = 0;
      return L4_EOK;
    }

  // ---- Regular file -------------------------------------------------------
  auto *file_svr = new(std::nothrow) Ext4_file_svr(path);
  if (!file_svr)
    return -L4_ENOMEM;

  L4::Cap<void> svr_cap = _registry->register_obj(file_svr);
  if (!svr_cap.is_valid())
    {
      delete file_svr;
      return -L4_ENOMEM;
    }

  snd_cap = L4::Ipc::Snd_fpage(svr_cap, L4_CAP_FPAGE_RWS);
  dummy.set_valid();
  out_name.set_valid();

  if (sep)
    {
      unsigned long rem = name.length - part - 1;
      memcpy(out_name->data, name.data + part + 1, rem);
      out_name->length = rem;
      return L4Re::Namespace::Partly_resolved;
    }

  out_name->length = 0;
  return L4_EOK;
}
