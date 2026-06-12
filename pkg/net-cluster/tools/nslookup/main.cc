/*
 * nslookup — DNS A-record lookup via netd.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Standalone net utility (net-cluster/tools): its own task, no lwIP, no sigma0.
 * netd owns the resolver; this tool just asks it (Net_svr::resolve) and prints.
 * Gets the "netd" cap via spawnd's cap-forwarding — `run rom/nslookup <host>`.
 */

#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <l4/re/env>

#include <net_ipc.h>

int main(int argc, char **argv)
{
  if (argc < 2)
    {
      printf("usage: nslookup <hostname>\n");
      return 1;
    }

  auto netd = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!netd.is_valid())
    {
      printf("nslookup: netd unavailable\n");
      return 1;
    }

  const char *host = argv[1];
  l4_uint32_t ip_be = 0;
  long r = netd->resolve(L4::Ipc::Array<char const>(strlen(host), host), ip_be);
  if (r != L4_EOK)
    {
      printf("nslookup: can't resolve '%s'\n", host);
      return 1;
    }

  struct in_addr a; a.s_addr = ip_be;
  printf("Server:  10.0.2.3\n\n");
  printf("Name:    %s\n", host);
  printf("Address: %s\n", inet_ntoa(a));
  return 0;
}
