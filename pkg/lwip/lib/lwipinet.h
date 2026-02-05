/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2022 Kernkonzept GmbH.
 * Author(s): Stephan Gerhold <stephan.gerhold@kernkonzept.com>
 */

#ifndef LWIP_HDR_EXTERNAL_INET_H
#define LWIP_HDR_EXTERNAL_INET_H

#include <arpa/inet.h>

#if LWIP_IPV4
#define inet_addr_from_ip4addr(target_inaddr, source_ipaddr) ((target_inaddr)->s_addr = ip4_addr_get_u32(source_ipaddr))
#define inet_addr_to_ip4addr(target_ipaddr, source_inaddr)   (ip4_addr_set_u32(target_ipaddr, (source_inaddr)->s_addr))
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
#define inet6_addr_from_ip6addr(target_in6addr, source_ip6addr) \
  { __builtin_memcpy((target_in6addr)->s6_addr, (source_ip6addr)->addr, sizeof((target_in6addr)->s6_addr)); }
#define inet6_addr_to_ip6addr(target_ip6addr, source_in6addr) \
  { __builtin_memcpy((target_ip6addr)->addr, (source_in6addr)->s6_addr32, sizeof((target_ip6addr)->addr)); ip6_addr_clear_zone(target_ip6addr); }
#endif /* LWIP_IPV6 */

#endif /* LWIP_HDR_EXTERNAL_INET_H */
