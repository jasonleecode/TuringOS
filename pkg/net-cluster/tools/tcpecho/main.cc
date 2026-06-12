/*
 * tcpecho — start a TCP echo server (in netd) for testing.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Replaces the old in-shell `net` builtin.  The echo server runs on a worker
 * thread inside netd (which owns lwIP); this tool just asks netd to start it
 * (Net_svr::tcp_echo) and prints the status.  `run rom/tcpecho [port]`.
 */

#include <cstdio>
#include <cstdlib>

#include <l4/re/env>

#include <net_ipc.h>

int main(int argc, char **argv)
{
  auto netd = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!netd.is_valid())
    {
      printf("tcpecho: netd unavailable\n");
      return 1;
    }

  l4_uint32_t port = (argc > 1) ? (l4_uint32_t)atoi(argv[1]) : 5000;

  char buf[256];
  L4::Ipc::Array<char> text(sizeof(buf), buf);
  long r = netd->tcp_echo(port, text);
  if (r != L4_EOK)
    {
      printf("tcpecho: failed (%ld)\n", r);
      return 1;
    }
  printf("%.*s\n", (int)text.length, text.data);
  return 0;
}
