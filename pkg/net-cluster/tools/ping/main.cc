/*
 * ping — ICMP echo via netd.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Standalone net utility (net-cluster/tools): its own task, no lwIP, no sigma0.
 * netd owns the raw ICMP socket; this tool resolves the host (Net_svr::resolve),
 * then drives one ICMP echo per packet (Net_svr::ping_one) — keeping each reply
 * within the inline IPC budget — and computes the summary itself.
 * `run rom/ping <host> [-c count]`.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <l4/re/env>
#include <l4/util/util.h>      /* l4_sleep */

#include <net_ipc.h>

int main(int argc, char **argv)
{
  if (argc < 2)
    {
      printf("usage: ping <host> [-c count]\n");
      return 1;
    }

  auto netd = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!netd.is_valid())
    {
      printf("ping: netd unavailable\n");
      return 1;
    }

  const char *host = argv[1];
  int count = 4;
  for (int i = 2; i < argc; i++)
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
      count = atoi(argv[++i]);
  if (count < 1) count = 1;

  l4_uint32_t ip_be = 0;
  long r = netd->resolve(L4::Ipc::Array<char const>(strlen(host), host), ip_be);
  if (r != L4_EOK)
    {
      printf("ping: can't resolve '%s'\n", host);
      return 1;
    }

  struct in_addr ia; ia.s_addr = ip_be;
  printf("PING %s (%s): %u data bytes\n", host, inet_ntoa(ia), 56u);

  int sent = 0, rcvd = 0;
  long min_us = 0x7fffffffL, max_us = 0, total_us = 0;
  char buf[256];

  for (int seq = 0; seq < count; seq++)
    {
      if (seq > 0) l4_sleep(1000);   /* ~1 s between echo requests */

      l4_int32_t rtt_us = -1;
      L4::Ipc::Array<char> line(sizeof(buf), buf);
      r = netd->ping_one(ip_be, (l4_uint32_t)seq, rtt_us, line);
      if (r != L4_EOK)
        {
          printf("ping: request failed (%ld)\n", r);
          continue;
        }
      sent++;
      printf("%.*s\n", (int)line.length, line.data);
      if (rtt_us >= 0)
        {
          rcvd++;
          if (rtt_us < min_us) min_us = rtt_us;
          if (rtt_us > max_us) max_us = rtt_us;
          total_us += rtt_us;
        }
    }

  printf("\n--- %s ping statistics ---\n", host);
  printf("%d packets transmitted, %d received, %d%% packet loss\n",
         sent, rcvd, sent ? (sent - rcvd) * 100 / sent : 0);
  if (rcvd > 0)
    printf("rtt min/avg/max = %ld.%ld/%ld.%ld/%ld.%ld ms\n",
           min_us / 1000, (min_us % 1000) / 100,
           (total_us / rcvd) / 1000, ((total_us / rcvd) % 1000) / 100,
           max_us / 1000, (max_us % 1000) / 100);
  return 0;
}
