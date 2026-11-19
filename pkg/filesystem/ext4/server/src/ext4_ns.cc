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

#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <new>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Round `v` up to the next power-of-2 `align`.
static inline size_t round_page(size_t v)
{ return (v + 4095UL) & ~4095UL; }

// --- Sub-namespace dedup cache --------------------------------------------
//
// A directory query (`ls`/`cd /ext4/sub`) used to `new` a fresh Ext4_namespace
// + register a new cap on *every* call, none ever freed → unbounded cap-slot
// growth on a long-running system.  Sub-namespaces are stateless routers (they
// only hold a path prefix), so we cache one object per distinct directory path
// and re-hand-out its existing cap.  This bounds the count to the number of
// directories actually traversed (finite), with no staleness risk.
//
// The ext4fs server is single-threaded, so an unsynchronised file-scoped list
// is safe.  Cache nodes are themselves bounded to #distinct directories.
namespace {

struct Subns_node
{
  Ext4_namespace *ns;
  Subns_node     *next;
};

static Subns_node *s_subns_cache = nullptr;

static Ext4_namespace *find_cached_subns(const char *path)
{
  for (Subns_node *n = s_subns_cache; n; n = n->next)
    if (strcmp(n->ns->prefix(), path) == 0)
      return n->ns;
  return nullptr;
}

static void cache_subns(Ext4_namespace *ns)
{
  auto *n = new(std::nothrow) Subns_node;
  if (!n) return;   // OOM here is harmless: next query just re-creates the sub-ns
  n->ns   = ns;
  n->next = s_subns_cache;
  s_subns_cache = n;
}

} // namespace

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
  ds.release();

  return L4_EOK;
}

// ---------------------------------------------------------------------------
// Path helper
// ---------------------------------------------------------------------------

bool Ext4_namespace::build_path(char *out, size_t size, const char *name, size_t n) const
{
  while (n && *name == '/') { ++name; --n; }
  if (!n || memchr(name, 0, n)) return false;
  size_t start = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i < n && name[i] != '/') continue;
    size_t len = i - start;
    if (!len || (len == 1 && name[start] == '.')
        || (len == 2 && !memcmp(name + start, "..", 2))) return false;
    start = i + 1;
  }
  int len = snprintf(out, size, "%s/%.*s", strcmp(_prefix, "/") ? _prefix : "", (int)n, name);
  return len >= 0 && (size_t)len < size && len < 256;
}

// ---------------------------------------------------------------------------
// op_unlink (L4Re::Namespace protocol — leaf name relative to _prefix)
// ---------------------------------------------------------------------------

l4_ret_t
Ext4_namespace::op_unlink(L4Re::Namespace::Rights,
                           Name_buffer const &name)
{
  char path[512];
  if (!build_path(path, sizeof(path), name.data, name.length)) return -L4_EINVAL;

  if (ext4_fremove(path) == EOK || ext4_dir_rm(path) == EOK)
    return ext4_cache_flush(path) == EOK ? L4_EOK : -L4_EIO;
  return -L4_ENOENT;
}

// ---------------------------------------------------------------------------
// op_ext_mkdir / op_ext_unlink (Ext4_dir_ops protocol — absolute ext4 paths)
// ---------------------------------------------------------------------------

l4_ret_t
Ext4_namespace::op_ext_mkdir(Ext4_dir_ops::Rights,
                              L4::Ipc::Array_ref<char const, unsigned long> const &path)
{
  char pbuf[512];
  if (!build_path(pbuf, sizeof(pbuf), path.data, path.length)) return -L4_EINVAL;

  if (ext4_inode_exist(pbuf,EXT4_DE_UNKNOWN) == EOK) return -L4_EEXIST;
  int r = ext4_dir_mk(pbuf);
  if (r != EOK)
    {
      printf("[ext4ns] mkdir '%s' failed: %d\n", pbuf, r);
      return (r == EEXIST) ? -L4_EEXIST : -L4_EIO;
    }
  if (ext4_cache_flush(pbuf) != EOK) return -L4_EIO;
  printf("[ext4ns] mkdir '%s' ok\n", pbuf);
  return L4_EOK;
}

l4_ret_t
Ext4_namespace::op_ext_unlink(Ext4_dir_ops::Rights,
                               L4::Ipc::Array_ref<char const, unsigned long> const &path)
{
  char pbuf[512];
  if (!build_path(pbuf, sizeof(pbuf), path.data, path.length)) return -L4_EINVAL;

  if (ext4_fremove(pbuf) == EOK || ext4_dir_rm(pbuf) == EOK)
    return ext4_cache_flush(pbuf) == EOK ? L4_EOK : -L4_EIO;
  printf("[ext4ns] unlink '%s' failed\n", pbuf);
  return -L4_ENOENT;
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
  if (!build_path(path, sizeof(path), name.data, part)) return -L4_EINVAL;

  // ---- Check if path is a directory ---------------------------------------
  // Return a child Ext4_namespace rooted at `path` so opendir() and ls work.
  // Reuse a cached sub-namespace for this path if one exists (dedup) so that
  // repeated `ls`/`cd` don't leak a server object + cap per call.
  ext4_dir probe;
  if (ext4_dir_open(&probe, path) == EOK)
    {
      ext4_dir_close(&probe);

      L4::Cap<void> sub_cap;
      if (Ext4_namespace *cached = find_cached_subns(path))
        sub_cap = cached->obj_cap();
      else
        {
          auto *sub_ns = new(std::nothrow) Ext4_namespace(_registry, path);
          if (!sub_ns)
            return -L4_ENOMEM;

          sub_cap = _registry->register_obj(sub_ns);
          if (!sub_cap.is_valid())
            {
              delete sub_ns;
              return -L4_ENOMEM;
            }
          cache_subns(sub_ns);
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
  // One Ext4_file_svr per open; reclaimed when the client calls op_release
  // (from its VFS destructor).  Pass the registry so it can unregister itself.
  auto *file_svr = new(std::nothrow) Ext4_file_svr(_registry, path);
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

long Ext4_namespace::op_ext_open(Ext4_dir_ops::Rights, L4::Ipc::Array_ref<char const,unsigned long> const &name,
                                 unsigned flags,unsigned &directory,L4::Ipc::Snd_fpage &object)
{
  char path[256];
  if (!build_path(path,sizeof(path),name.data,name.length)) return -L4_EINVAL;
  directory = ext4_inode_exist(path,EXT4_DE_DIR)==EOK;
  if (directory) {
    if ((flags & O_ACCMODE) != O_RDONLY) return -EISDIR;
    object = L4::Ipc::Snd_fpage(obj_cap(), L4_CAP_FPAGE_R);
    return 0;
  }
  if (flags & O_DIRECTORY) return -ENOTDIR;
  if ((flags & (O_CREAT|O_EXCL)) == (O_CREAT|O_EXCL) && ext4_inode_exist(path,EXT4_DE_REG_FILE)==EOK)
    return -EEXIST;
  auto file = new(std::nothrow) Ext4_file_svr(_registry,path);
  if (!file) return -ENOMEM;
  int rc = file->open_file(flags & (O_ACCMODE|O_CREAT|O_TRUNC));
  if (rc) { delete file; return -rc; }
  auto cap = _registry->register_obj(file);
  if (!cap.is_valid()) { delete file; return -ENOMEM; }
  object = L4::Ipc::Snd_fpage(cap,L4_CAP_FPAGE_RWS);
  return 0;
}

long Ext4_namespace::op_ext_remove(Ext4_dir_ops::Rights,
                                   L4::Ipc::Array_ref<char const,unsigned long> const &name,
                                   unsigned directory)
{
  char path[256];
  if (!build_path(path,sizeof(path),name.data,name.length)) return -EINVAL;
  int rc;
  if (directory) {
    ext4_dir dir;
    rc = ext4_dir_open(&dir,path);
    if (rc != EOK) return -rc;
    bool empty = true;
    while (const ext4_direntry *e = ext4_dir_entry_next(&dir)) {
      if (e->name_length == 1 && e->name[0] == '.') continue;
      if (e->name_length == 2 && !memcmp(e->name,"..",2)) continue;
      empty = false;
      break;
    }
    ext4_dir_close(&dir);
    if (!empty) return -ENOTEMPTY;
    rc = ext4_dir_rm(path);
  } else {
    if (ext4_inode_exist(path,EXT4_DE_DIR) == EOK) return -EISDIR;
    rc = ext4_fremove(path);
  }
  return rc == EOK ? -ext4_cache_flush(path) : -rc;
}
