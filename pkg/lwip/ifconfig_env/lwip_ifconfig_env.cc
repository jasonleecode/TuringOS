/*
 * Copyright (C) 2022 Kernkonzept GmbH.
 * Author(s): Stephan Gerhold <stephan.gerhold@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <lwip/dhcp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

enum
{
  Max_env_len = sizeof("IFCONFIG_IP4_") + NETIF_NAMESIZE,
};

struct Ifconfig_env
{
  Ifconfig_env();

  template<typename FUNC>
  static void for_each_netif(FUNC func);

  static const char *get_ifconfig(const char *ifname);

  static void configure_ip4(netif *netif, const char *val);

  static void wait_for_dhcp();
};

Ifconfig_env::Ifconfig_env()
{
  for_each_netif(Ifconfig_env::configure_ip4);
  wait_for_dhcp();
}

template<typename FUNC>
void
Ifconfig_env::for_each_netif(FUNC func)
{
  char name[NETIF_NAMESIZE];
  netif *netif;

  LOCK_TCPIP_CORE();
  NETIF_FOREACH(netif)
    {
      func(netif, netif_index_to_name(netif_get_index(netif), name));
    }
  UNLOCK_TCPIP_CORE();
}

const char *
Ifconfig_env::get_ifconfig(const char *ifname)
{
  char env[Max_env_len] = "IFCONFIG_IP4_";
  strcat(env, ifname);
  return getenv(env);
}

static u8_t *ip4_addr_p(ip4_addr_t *ip, int idx)
{ return &reinterpret_cast<u8_t *>(&ip->addr)[idx]; }

void
Ifconfig_env::configure_ip4(netif *netif, const char *ifname)
{
  const char *ifconfig = get_ifconfig(ifname);
  if (!ifconfig)
    return;

  // IFCONFIG_IP4_vn0 "192.168.42.2/24 via 192.168.42.1" or "dhcp"
  if (strcmp(ifconfig, "dhcp") == 0)
    {
      dhcp_start(netif);

      return;
    }

  ip4_addr_t ip, netmask, gw;
  auto prefix = 32U;

  auto n = sscanf(ifconfig, "%hhu.%hhu.%hhu.%hhu/%u via %hhu.%hhu.%hhu.%hhu",
                  ip4_addr_p(&ip, 0), ip4_addr_p(&ip, 1),
                  ip4_addr_p(&ip, 2), ip4_addr_p(&ip, 3),
                  &prefix,
                  ip4_addr_p(&gw, 0), ip4_addr_p(&gw, 1),
                  ip4_addr_p(&gw, 2), ip4_addr_p(&gw, 3));

  if (n <= 8)
    ip4_addr_set_any(&gw);
  if (n < 4)
    ip4_addr_set_any(&ip);
  ip4_addr_set_u32(&netmask, lwip_htonl(~0UL << (32U - std::min(prefix, 32U))));

  netif_set_addr(netif, &ip, &netmask, &gw);
  printf("Interface '%s' configured statically:\n", ifname);
  printf("  IP address: %s\n", ip4addr_ntoa(&ip));
  printf("  Netmask: %s\n", ip4addr_ntoa(&netmask));
  printf("  Gateway: %s\n", ip4addr_ntoa(&gw));
}

void
Ifconfig_env::wait_for_dhcp()
{
  // TODO: Timeout after x seconds?
  bool printed = false;
  for (;;)
    {
      bool dhcp_ongoing = false;

      for_each_netif(
        [&dhcp_ongoing](netif *netif, const char *ifname)
        {
          const char *ifconfig = get_ifconfig(ifname);
          if (!ifconfig || strcmp(ifconfig, "dhcp") != 0)
            return;

          if (!dhcp_supplied_address(netif))
            dhcp_ongoing = true;
        });

      if (!dhcp_ongoing)
        break;

      if (!printed)
        {
          printf("Waiting for DHCP to configure interfaces...\n");
          printed = true;
        }

      // Sleep for 50 ms, then check again.
      usleep(50'000);
    };

  for_each_netif(
    [](netif *netif, const char *ifname)
    {
      const char *ifconfig = get_ifconfig(ifname);
      if (!ifconfig || strcmp(ifconfig, "dhcp") != 0)
        return;

      struct dhcp *dhcp = netif_dhcp_data(netif);
      printf("Interface '%s' configured via DHCP:\n", ifname);
      printf("  IP address: %s\n", ip4addr_ntoa(&dhcp->offered_ip_addr));
      printf("  Netmask: %s\n", ip4addr_ntoa(&dhcp->offered_sn_mask));
      printf("  Gateway: %s\n", ip4addr_ntoa(&dhcp->offered_gw_addr));
    });
}

Ifconfig_env ifce;
