/*
 * TuringOS ext4fs — per-file IPC server object (streaming I/O)
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

Ext4_file_svr::Ext4_file_svr(L4Re::Util::Object_registry *registry,
                             const char *path)
: _registry(registry)
{
  strncpy(_path, path, sizeof(_path) - 1);
  _path[sizeof(_path) - 1] = '\0';

  // Allocate the fixed bounce buffer and map it into our address space.  The
  // same DS cap is handed to the client in op_setup; both sides map it and
  // transfer data through it.  No whole-file buffering.
  _buf = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
  if (!_buf.is_valid())
    { printf("[ext4svr] cap alloc failed for '%s'\n", _path); return; }

  long r = L4Re::Env::env()->mem_alloc()->alloc(BUF_SIZE, _buf.get());
  if (r < 0)
    { printf("[ext4svr] buffer alloc failed for '%s': %ld\n", _path, r); return; }

  r = L4Re::Env::env()->rm()->attach(
        &_buf_addr, BUF_SIZE,
        L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
        L4::Ipc::make_cap_rw(_buf.get()), 0);
  if (r < 0)
    { _buf_addr = 0; printf("[ext4svr] buffer attach failed for '%s': %ld\n", _path, r); return; }

  _ok = true;
}

Ext4_file_svr::~Ext4_file_svr()
{
  if (_f_open)
    { ext4_fclose(&_f); _f_open = false; }
  if (_buf_addr)
    L4Re::Env::env()->rm()->detach(_buf_addr, nullptr);
  // _buf Unique_cap frees the cap slot.
}

// Open the file on first use, keeping it open for the handle's lifetime.
// O_RDWR|O_CREAT preserves existing content (truncation, if requested, is
// applied separately via op_ftruncate driven by the client's O_TRUNC).
bool Ext4_file_svr::ensure_open()
{
  if (_f_open)
    return true;
  if (ext4_fopen2(&_f, _path, O_RDWR | O_CREAT) != EOK)
    {
      printf("[ext4svr] fopen '%s' failed\n", _path);
      return false;
    }
  _f_open = true;
  return true;
}

l4_ret_t
Ext4_file_svr::op_setup(Ext4_file_ops::Rights,
                        L4::Ipc::Snd_fpage &snd_buf,
                        l4_uint64_t &size,
                        l4_uint32_t &buf_size)
{
  if (!_ok || !_buf.is_valid())
    return -L4_ENOMEM;
  if (!ensure_open())
    return -L4_EIO;

  size     = ext4_fsize(&_f);
  buf_size = BUF_SIZE;
  snd_buf  = L4::Ipc::Snd_fpage(L4::Cap<void>(_buf.get().cap()), L4_CAP_FPAGE_RW);
  return L4_EOK;
}

l4_ret_t
Ext4_file_svr::op_pread(Ext4_file_ops::Rights,
                        l4_uint64_t off, l4_uint32_t len, l4_uint32_t &got)
{
  got = 0;
  if (!_ok || !_buf_addr) return -L4_ENOMEM;
  if (!ensure_open())     return -L4_EIO;
  if (len > BUF_SIZE)     len = BUF_SIZE;

  if (ext4_fseek(&_f, (int64_t)off, SEEK_SET) != EOK)
    return -L4_EIO;

  size_t rc = 0;
  int re = ext4_fread(&_f, reinterpret_cast<void *>(_buf_addr), len, &rc);
  if (re != EOK)
    {
      printf("[ext4svr] pread '%s' off=%llu len=%u failed: %d\n",
             _path, (unsigned long long)off, len, re);
      return -L4_EIO;
    }
  got = (l4_uint32_t)rc;   // 0 == EOF
  return L4_EOK;
}

l4_ret_t
Ext4_file_svr::op_pwrite(Ext4_file_ops::Rights,
                         l4_uint64_t off, l4_uint32_t len, l4_uint32_t &put)
{
  put = 0;
  if (!_ok || !_buf_addr) return -L4_ENOMEM;
  if (!ensure_open())     return -L4_EIO;
  if (len > BUF_SIZE)     len = BUF_SIZE;

  if (ext4_fseek(&_f, (int64_t)off, SEEK_SET) != EOK)
    return -L4_EIO;

  size_t wc = 0;
  int re = ext4_fwrite(&_f, reinterpret_cast<void *>(_buf_addr), len, &wc);
  if (re != EOK || wc != len)
    {
      printf("[ext4svr] pwrite '%s' off=%llu len=%u failed: re=%d wc=%zu\n",
             _path, (unsigned long long)off, len, re, wc);
      return -L4_EIO;
    }
  put = (l4_uint32_t)wc;
  return L4_EOK;
}

l4_ret_t
Ext4_file_svr::op_pappend(Ext4_file_ops::Rights,
                          l4_uint32_t len, l4_uint32_t &put, l4_uint64_t &off)
{
  put = 0;
  off = 0;
  if (!_ok || !_buf_addr) return -L4_ENOMEM;
  if (!ensure_open())     return -L4_EIO;
  if (len > BUF_SIZE)     len = BUF_SIZE;

  // Seek to EOF and capture that offset, then write — all within this one
  // synchronous RPC, which the single-threaded server runs to completion
  // before serving the next.  So two concurrent O_APPEND writers each land at
  // the then-current EOF and neither clobbers the other.
  if (ext4_fseek(&_f, 0, SEEK_END) != EOK)
    return -L4_EIO;
  off = ext4_fsize(&_f);

  size_t wc = 0;
  int re = ext4_fwrite(&_f, reinterpret_cast<void *>(_buf_addr), len, &wc);
  if (re != EOK || wc != len)
    {
      printf("[ext4svr] pappend '%s' len=%u failed: re=%d wc=%zu\n",
             _path, len, re, wc);
      return -L4_EIO;
    }
  put = (l4_uint32_t)wc;
  return L4_EOK;
}

l4_ret_t
Ext4_file_svr::op_ftruncate(Ext4_file_ops::Rights, l4_uint64_t size)
{
  if (!_ok)           return -L4_ENOMEM;
  if (!ensure_open()) return -L4_EIO;

  if (ext4_ftruncate(&_f, size) != EOK)
    {
      printf("[ext4svr] ftruncate '%s' size=%llu failed\n",
             _path, (unsigned long long)size);
      return -L4_EIO;
    }
  return L4_EOK;
}

l4_ret_t
Ext4_file_svr::op_release(Ext4_file_ops::Rights)
{
  // Client is done with this handle.  Close the ext4 file (flushes lwext4's
  // cache for it), detach + free the bounce buffer, unregister from the server
  // (frees the IPC gate + cap slot, routes in-flight senders to the null
  // handler), then self-destruct.
  //
  // Self-deletion is safe inside this handler: Server::internal_loop sends the
  // reply for the current RPC from the UTCB at the top of the *next* loop
  // iteration, and release returns only a status word, so the dispatcher never
  // dereferences this object after we return.
  if (_f_open)
    { ext4_fclose(&_f); _f_open = false; }
  if (_buf_addr)
    { L4Re::Env::env()->rm()->detach(_buf_addr, nullptr); _buf_addr = 0; }
  _ok = false;

  if (_registry)
    _registry->unregister_obj(this);

  printf("[ext4svr] op_release: freed handle for '%s'\n", _path);
  delete this;
  return L4_EOK;
}
