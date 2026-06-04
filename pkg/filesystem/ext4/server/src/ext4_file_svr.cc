/*
 * TuringOS ext4fs — per-file IPC server object (Phase 4)
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */

#include "ext4_file_svr.h"

#include <l4/re/env>
#include <l4/re/mem_alloc>
#include <l4/re/rm>

extern "C" {
#include <ext4.h>
#include <ext4_errno.h>
}

#include <string.h>
#include <stdio.h>

// Maximum DS size for a single file (4 MiB).
static const unsigned long FILE_DS_MAX = 4u << 20;

// Round `v` up to the next multiple of `align` (must be power-of-2).
static inline unsigned long round_up(unsigned long v, unsigned long align)
{ return (v + align - 1) & ~(align - 1); }

Ext4_file_svr::Ext4_file_svr(L4Re::Util::Object_registry *registry,
                             const char *path)
: _registry(registry)
{
  strncpy(_path, path, sizeof(_path) - 1);
  _path[sizeof(_path) - 1] = '\0';

  // Check whether the file exists and get its size.
  ext4_file f;
  bool exists = (ext4_fopen(&f, _path, "rb") == EOK);
  if (exists)
    {
      _fsize = ext4_fsize(&f);
      ext4_fclose(&f);
    }

  // Allocate the Dataspace (at least one page, cap at FILE_DS_MAX).
  _ds_size = round_up(_fsize ? (unsigned long)_fsize : 1UL, 4096UL);
  if (_ds_size < 4096UL)         _ds_size = 4096UL;
  if (_ds_size > FILE_DS_MAX)    _ds_size = FILE_DS_MAX;

  _ds = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
  if (!_ds.is_valid())
    { printf("[ext4svr] cap alloc failed for '%s'\n", _path); return; }

  long r = L4Re::Env::env()->mem_alloc()->alloc(_ds_size, _ds.get());
  if (r < 0)
    { printf("[ext4svr] mem_alloc failed for '%s': %ld\n", _path, r); return; }

  // Map the DS into our own address space.
  _ds_addr = 0;
  r = L4Re::Env::env()->rm()->attach(
        &_ds_addr, _ds_size,
        L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
        L4::Ipc::make_cap_rw(_ds.get()), 0);
  if (r < 0)
    { printf("[ext4svr] rm attach failed for '%s': %ld\n", _path, r); return; }

  // Pre-fill with existing file content.
  if (exists && _fsize > 0)
    {
      unsigned long to_read =
        (_fsize <= _ds_size) ? (unsigned long)_fsize : _ds_size;

      if (ext4_fopen(&f, _path, "rb") == EOK)
        {
          size_t rd = 0;
          int re = ext4_fread(&f, reinterpret_cast<void *>(_ds_addr), to_read, &rd);
          ext4_fclose(&f);
          if (re != EOK || rd != to_read)
            {
              printf("[ext4svr] fread '%s' failed: r=%d rd=%zu want=%lu\n",
                     _path, re, rd, to_read);
              return;
            }
          const unsigned char *p = reinterpret_cast<unsigned char *>(_ds_addr);
          printf("[ext4svr] loaded '%s' %zu bytes [%02x %02x %02x %02x]\n",
                 _path, rd, p[0], p[1], p[2], p[3]);
        }
      else
        {
          printf("[ext4svr] fopen '%s' for read failed\n", _path);
          return;
        }
    }

  _ok = true;
}

Ext4_file_svr::~Ext4_file_svr()
{
  if (_ds_addr)
    L4Re::Env::env()->rm()->detach(_ds_addr, nullptr);
  // _ds Unique_cap automatically frees the cap slot.
}

l4_ret_t
Ext4_file_svr::op_get_ds(Ext4_file_ops::Rights,
                           L4::Ipc::Snd_fpage &snd_ds,
                           l4_uint64_t &size)
{
  if (!_ok || !_ds.is_valid())
    return -L4_ENOMEM;

  snd_ds = L4::Ipc::Snd_fpage(L4::Cap<void>(_ds.get().cap()), L4_CAP_FPAGE_RW);
  size   = _fsize;
  return L4_EOK;
}

l4_ret_t
Ext4_file_svr::op_close(Ext4_file_ops::Rights, l4_uint64_t written)
{
  if (!_ok || !_ds_addr)
    return L4_EOK;

  // written == 0 means "commit nothing" (e.g. read-only handle, or fsync with
  // no dirty bytes).  Do NOT open "wb" here — that would truncate the file to
  // empty.  Just skip the flush.  This commit may run multiple times over the
  // handle's life (fsync + final fclose), so it must not destroy the object;
  // teardown happens in op_release.
  if (written == 0)
    return L4_EOK;

  // "wb" truncates before writing — correct for both overwrite and new files.
  ext4_file f;
  if (ext4_fopen(&f, _path, "wb") != EOK)
    {
      printf("[ext4svr] op_close: cannot open '%s' for write\n", _path);
      return -L4_EIO;
    }

  unsigned long to_write =
    (written <= _ds_size) ? (unsigned long)written : _ds_size;
  size_t wr = 0;
  int re = ext4_fwrite(&f, reinterpret_cast<void *>(_ds_addr), to_write, &wr);
  ext4_fclose(&f);
  if (re != EOK || wr != to_write)
    {
      printf("[ext4svr] op_close: write '%s' failed (r=%d wr=%zu)\n",
             _path, re, wr);
      return -L4_EIO;
    }

  printf("[ext4svr] op_close: wrote %zu bytes → '%s'\n", wr, _path);
  return L4_EOK;
}

l4_ret_t
Ext4_file_svr::op_release(Ext4_file_ops::Rights)
{
  // Client is done with this handle.  Detach our local DS mapping, unregister
  // from the server (frees the IPC gate + internally-allocated cap slot and
  // routes any in-flight senders to the null handler), then self-destruct.
  //
  // Self-deletion is safe inside this handler: the server loop's reply for the
  // current RPC is sent from the UTCB at the top of the *next* loop iteration
  // (Server::internal_loop), and release returns only a status word, so the
  // dispatcher never dereferences this object after we return.  The ~dtor
  // detaches the mapping and frees the DS cap via Unique_cap.
  if (_ds_addr)
    {
      L4Re::Env::env()->rm()->detach(_ds_addr, nullptr);
      _ds_addr = 0;
    }
  _ok = false;

  if (_registry)
    _registry->unregister_obj(this);

  printf("[ext4svr] op_release: freed handle for '%s'\n", _path);
  delete this;
  return L4_EOK;
}
