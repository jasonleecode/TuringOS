/*
 * netcat — a standalone TCP client, the network reached purely through netd.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Decompose-native_shell step ③ (net apps as separate processes), first cut.
 * This is its own task: it links no lwIP and holds no sigma0 — it opens a TCP
 * connection, sends a message and prints the reply entirely over the Net_svr
 * IPC interface (net_ipc.h) exposed by the netd server.  It receives the "netd"
 * capability because spawnd forwards its own initial caps (including netd) by
 * name to every program it launches, so `run rom/netcat ...` just works.
 *
 * Usage:  netcat <ip> <port> <msg>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <l4/re/env>

#include <net_ipc.h>

int main(int argc, char **argv)
{
  if (argc < 4)
    {
      printf("usage: netcat <ip> <port> <msg>\n");
      return 1;
    }

  auto netd = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!netd.is_valid())
    {
      printf("netcat: netd unavailable\n");
      return 1;
    }

  l4_uint32_t ip_be = inet_addr(argv[1]);
  if (ip_be == INADDR_NONE)
    {
      printf("netcat: bad IP '%s'\n", argv[1]);
      return 1;
    }
  l4_uint16_t port = (l4_uint16_t)atoi(argv[2]);
  const char *msg  = argv[3];

  l4_uint32_t handle = 0;
  long r = netd->tcp_connect(ip_be, port, handle);
  if (r != L4_EOK)
    {
      printf("netcat: connect to %s:%u failed (%ld)\n", argv[1], port, r);
      return 1;
    }

  l4_uint32_t sent = 0;
  r = netd->send(handle, L4::Ipc::Array<char const>(strlen(msg), msg), sent);
  if (r != L4_EOK)
    {
      printf("netcat: send failed (%ld)\n", r);
      netd->close(handle);
      return 1;
    }
  printf("netcat: sent %u bytes to %s:%u\n", sent, argv[1], port);

  char buf[1600];
  L4::Ipc::Array<char> rx(sizeof(buf), buf);
  r = netd->recv(handle, sizeof(buf), rx);
  if (r != L4_EOK)
    {
      printf("netcat: recv failed (%ld)\n", r);
      netd->close(handle);
      return 1;
    }

  if (rx.length == 0)
    printf("netcat: peer closed (no data)\n");
  else
    printf("netcat: recv %u bytes: %.*s\n",
           (unsigned)rx.length, (int)rx.length, rx.data);

  netd->close(handle);
  return 0;
}
