/*
 * Copyright (C) 2024 Kernkonzept GmbH.
 * Author(s): Georg Kotheimer <georg.kotheimer@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <lwipopts.h>
#include <lwipinet.h>
#include <lwipsockets.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

#include <ifaddrs.h>
#include <string.h>

struct ifaddrs_storage
{
  struct ifaddrs ifa;
  char name[NETIF_NAMESIZE];
  union
  {
    struct sockaddr sa;
    struct sockaddr_in ip4;
    struct sockaddr_in6 ip6;
  } addr, netmask;
};

#if LWIP_IPV4
static void
ip4addr_port_to_sockaddr(struct sockaddr_in *sin, ip4_addr_t const *ipaddr,
                         uint16_t port)
{
  sin->sin_family = AF_INET;
  sin->sin_port = lwip_htons(port);
  inet_addr_from_ip4addr(&sin->sin_addr, ipaddr);
  memset(sin->sin_zero, 0, SIN_ZERO_LEN);
}
#endif

#if LWIP_IPV6
static void
ip6addr_port_to_sockaddr(struct sockaddr_in6 *sin6, ip6_addr_t const *ipaddr,
                         uint16_t port)
{
      sin6->sin6_family = AF_INET6;
      sin6->sin6_port = lwip_htons((port));
      sin6->sin6_flowinfo = 0;
      inet6_addr_from_ip6addr(&sin6->sin6_addr, ipaddr);
      sin6->sin6_scope_id = ip6_addr_zone(ipaddr);
}
#endif

static struct ifaddrs_storage *
next_ifaddr_storage(struct netif *netif, struct ifaddrs_storage *ifas,
                    int *next_addr, int num_addrs)
{
  struct ifaddrs_storage *ifa = &ifas[(*next_addr)++];
  ifa->ifa.ifa_next = *next_addr < num_addrs ? &ifas[*next_addr].ifa : NULL;
  netif_index_to_name(netif_get_index(netif), ifa->name);
  ifa->ifa.ifa_name = ifa->name;
  return ifa;
}

int
getifaddrs(struct ifaddrs **ifap)
{
  int num_addrs = 0;
  int next_addr = 0;
  struct netif *netif;

  LOCK_TCPIP_CORE();

  NETIF_FOREACH(netif)
  {
    num_addrs++;

#if LWIP_IPV6
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++)
      if (ip6_addr_isvalid(netif_ip6_addr_state(netif, i)))
        num_addrs++;
#endif
  }

  if (!num_addrs)
    goto err_unlock;

  struct ifaddrs_storage *ifas = calloc(num_addrs, sizeof(struct ifaddrs_storage));
  if (!ifas)
    goto err_unlock;

  NETIF_FOREACH(netif)
  {
    struct ifaddrs_storage *ifa = NULL;

#if LWIP_IPV4
    if (!ip4_addr_isany(netif_ip4_addr(netif)))
      {
        ifa = next_ifaddr_storage(netif, ifas, &next_addr, num_addrs);
        ip4addr_port_to_sockaddr(&ifa->addr.ip4, netif_ip4_addr(netif), 0);
        ifa->ifa.ifa_addr = &ifa->addr.sa;
        ip4addr_port_to_sockaddr(&ifa->netmask.ip4, netif_ip4_netmask(netif), 0);
        ifa->ifa.ifa_netmask = &ifa->netmask.sa;
      }
#endif

#if LWIP_IPV6
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++)
      if (ip6_addr_isvalid(netif_ip6_addr_state(netif, i)))
        {
          ifa = next_ifaddr_storage(netif, ifas, &next_addr, num_addrs);
          ip6addr_port_to_sockaddr(&ifa->addr.ip6, netif_ip6_addr(netif, i), 0);
          ifa->ifa.ifa_addr = &ifa->addr.sa;
        }
#endif

    // In case the interface has neither no IP address assigned.
    if (ifa == NULL)
      ifa = next_ifaddr_storage(netif, ifas, &next_addr, num_addrs);
  }

  *ifap = &ifas[0].ifa;
  UNLOCK_TCPIP_CORE();
  return 0;

err_unlock:
  UNLOCK_TCPIP_CORE();
  return -1;
}

void freeifaddrs(struct ifaddrs *ifa)
{
  free(ifa);
}
