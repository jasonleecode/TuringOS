/*
 * dhcp — DHCP control via netd.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Standalone net utility (net-cluster/tools): its own task, no lwIP, no sigma0.
 * netd owns the DHCP client; this tool drives it (Net_svr::dhcp) and prints the
 * result.  `run rom/dhcp [release|status]` (default: renew).
 */

#include <cstdio>
#include <cstring>

#include <l4/re/env>

#include <net_ipc.h>

int main(int argc, char **argv)
{
  auto netd = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!netd.is_valid())
    {
      printf("dhcp: netd unavailable\n");
      return 1;
    }

  l4_uint32_t action = 0;                      /* renew */
  if (argc > 1 && strcmp(argv[1], "release") == 0) action = 1;
  else if (argc > 1 && strcmp(argv[1], "status") == 0) action = 2;

  char buf[2000];
  L4::Ipc::Array<char> text(sizeof(buf), buf);
  long r = netd->dhcp(action, text);
  if (r != L4_EOK)
    {
      printf("dhcp: failed (%ld)\n", r);
      return 1;
    }
  fwrite(text.data, 1, text.length, stdout);
  return 0;
}
