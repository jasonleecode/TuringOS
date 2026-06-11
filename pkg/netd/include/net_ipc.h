/*
 * TuringOS — netd socket-over-IPC protocol.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Protocol 0x5904: the network-service interface between netd (which owns the
 * virtio-net NIC + lwIP) and its clients.  This is Phase 1 of decomposing
 * native_shell's in-process network stack into an isolated server: netd holds
 * the NIC (via vbus, NOT sigma0) and the whole TCP/IP stack; clients use the
 * network purely over this IPC, so they no longer need lwIP or raw physical
 * memory access.
 *
 * Phase 1 is a minimal TCP *client* surface (connect / send / recv / close)
 * with inline data — enough to prove the architecture end-to-end.  Listen/
 * accept, UDP, DNS, poll, shared-dataspace bulk transfer and a transparent
 * libc socket backend are later phases.
 *
 * Protocol number map: 0x5901 spawnd, 0x5902 Temp_svr, 0x5903 Radio_svr,
 * 0x5904 = Net_svr (this).
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_iface>
#include <l4/sys/cxx/ipc_array>

struct Net_svr : L4::Kobject_t<Net_svr, L4::Kobject, 0x5904>
{
  /*
   * Open a TCP connection to ip_be:port and return an opaque handle.
   *   in  ip_be:  destination IPv4 address in network byte order
   *               (e.g. the result of inet_addr("10.0.2.2"))
   *   in  port:   destination TCP port in host byte order
   *   out handle: opaque connection handle for send/recv/close
   * Returns L4_EOK on success, or a negative L4 error code.
   */
  L4_INLINE_RPC(long, tcp_connect,
                (l4_uint32_t ip_be, l4_uint16_t port, l4_uint32_t &handle));

  /*
   * Send up to ~2 KiB of inline data on a connection.
   *   out sent: number of bytes actually written
   */
  L4_INLINE_RPC(long, send,
                (l4_uint32_t handle, L4::Ipc::Array<char const> data,
                 l4_uint32_t &sent));

  /*
   * Receive up to max (≤ ~2 KiB) bytes (blocking until data, EOF or error).
   *   out data: the received bytes (length 0 means peer closed)
   */
  L4_INLINE_RPC(long, recv,
                (l4_uint32_t handle, l4_uint32_t max,
                 L4::Ipc::Array<char> &data));

  /* Close a connection handle. */
  L4_INLINE_RPC(long, close, (l4_uint32_t handle));

  /*
   * Report the network interface(s) as preformatted, human-readable text
   * (ifconfig-style: flags, mtu, inet/netmask/broadcast, gateway, ether).
   *   out text: the formatted listing (empty if the stack isn't up)
   * netd owns lwIP, so clients (e.g. the `ifconfig` tool) don't need it.
   */
  L4_INLINE_RPC(long, ifconfig, (L4::Ipc::Array<char> &text));

  typedef L4::Typeid::Rpcs<tcp_connect_t, send_t, recv_t, close_t,
                           ifconfig_t> Rpcs;
};
