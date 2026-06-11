/*
 * ifconfig — show network interfaces, the data fetched from netd over IPC.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * A standalone net utility (net-cluster/tools): its own task, links no lwIP and
 * holds no sigma0.  netd owns the TCP/IP stack and the NIC; this tool just asks
 * netd for the interface listing (Net_svr::ifconfig) and prints it.  It gets the
 * "netd" capability because spawnd forwards its initial caps to every program it
 * launches, so `run rom/ifconfig` just works.
 */

#include <cstdio>

#include <l4/re/env>

#include <net_ipc.h>

int main()
{
  auto netd = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!netd.is_valid())
    {
      printf("ifconfig: netd unavailable\n");
      return 1;
    }

  char buf[1024];
  L4::Ipc::Array<char> text(sizeof(buf), buf);
  long r = netd->ifconfig(text);
  if (r != L4_EOK)
    {
      printf("ifconfig: query failed (%ld)\n", r);
      return 1;
    }

  if (text.length == 0)
    {
      printf("ifconfig: no interfaces up (network stack not ready)\n");
      return 0;
    }

  fwrite(text.data, 1, text.length, stdout);
  return 0;
}
