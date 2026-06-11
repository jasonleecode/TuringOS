/*
 * TuringOS — authentication service IPC protocol.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Protocol 0x5905: the credential-check interface between authd (which owns the
 * credential store) and its clients (native_shell's login).  This is step ②
 * of decomposing native_shell: the shell no longer contains any password — it
 * reads a username/password from the console and asks authd to verify them over
 * IPC.  authd keeps salted SHA-256 hashes in /ext4/etc/shadow, so the secret
 * lives in one small isolated service backed by a file, not in the big shell
 * binary.
 *
 * Protocol number map: 0x5901 spawnd, 0x5902 Temp, 0x5903 Radio, 0x5904 Net,
 * 0x5905 = Auth_svr (this).
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_iface>
#include <l4/sys/cxx/ipc_array>

struct Auth_svr : L4::Kobject_t<Auth_svr, L4::Kobject, 0x5905>
{
  /*
   * Verify a username/password pair.
   *   in user: the username
   *   in pass: the cleartext password (compared against the salted hash)
   * Returns L4_EOK if the credentials are valid, -L4_EPERM otherwise.
   */
  L4_INLINE_RPC(long, authenticate,
                (L4::Ipc::Array<char const> user,
                 L4::Ipc::Array<char const> pass));

  typedef L4::Typeid::Rpcs<authenticate_t> Rpcs;
};
