/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * Copyright (C) 2015, 2022 Kernkonzept GmbH.
 * Author(s): Stephan Gerhold <stephan.gerhold@kernkonzept.com>
 */

/**
 * @file
 *
 * lwIP Options Configuration
 */

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H
#ifdef __cplusplus
extern "C" {
#endif

#define LWIP_L4                         1
#define LWIP_DEBUG                      0

/*
   ------------------------------------
   ----------- Core locking -----------
   ------------------------------------
*/

#if !NO_SYS
void sys_check_core_locking(void);
#define LWIP_ASSERT_CORE_LOCKED()  sys_check_core_locking()
#endif

/*
   ------------------------------------
   ---------- Memory options ----------
   ------------------------------------
*/
/**
 * MEM_LIBC_MALLOC==1: Use malloc/free/realloc provided by your C-library
 * instead of the lwip internal allocator. Can save code size if you
 * already use it.
 */
#define MEM_LIBC_MALLOC                 1

/**
 * MEMP_MEM_MALLOC==1: Use mem_malloc/mem_free instead of the lwip pool allocator.
 * Especially useful with MEM_LIBC_MALLOC but handle with care regarding execution
 * speed (heap alloc can be much slower than pool alloc) and usage from interrupts
 * (especially if your netif driver allocates PBUF_POOL pbufs for received frames
 * from interrupt)!
 * ATTENTION: Currently, this uses the heap for ALL pools (also for private pools,
 * not only for internal pools defined in memp_std.h)!
 */
#define MEMP_MEM_MALLOC                 1

/**
 * MEM_ALIGNMENT: should be set to the alignment of the CPU
 *    4 byte alignment -> \#define MEM_ALIGNMENT 4
 *    2 byte alignment -> \#define MEM_ALIGNMENT 2
 */
#define MEM_ALIGNMENT                   4

/** LWIP_SUPPORT_CUSTOM_PBUF==1: Custom pbufs behave much like their pbuf type
 * but they are allocated by external code (initialised by calling
 * pbuf_alloced_custom()) and when pbuf_free gives up their last reference, they
 * are freed by calling pbuf_custom->custom_free_function().
 * Currently, the pbuf_custom code is only needed for one specific configuration
 * of IP_FRAG, unless required by external driver/application code. */
#define LWIP_SUPPORT_CUSTOM_PBUF         1

/*
   ------------------------------------------------
   ---------- Internal Memory Pool Sizes ----------
   ------------------------------------------------
*/

/**
 * MEMP_NUM_NETCONN: the number of struct netconns.
 * (only needed if you use the sequential API, like api_lib.c)
 */
#define MEMP_NUM_NETCONN                64

/*
   ---------------------------------
   ---------- ARP options ----------
   ---------------------------------
*/

/**
 * LWIP_ARP==1: Enable ARP functionality.
 */
#define LWIP_ARP                        1

/*
   --------------------------------
   ---------- IP options ----------
   --------------------------------
*/

/**
 * LWIP_IPV4==1: Enable IPv4
 */
#define LWIP_IPV4                       1

/*
   ---------------------------------
   ---------- RAW options ----------
   ---------------------------------
*/

/**
 * LWIP_RAW==1: Enable application layer to hook into the IP layer itself.
 */
#define LWIP_RAW                        1

/*
   ----------------------------------
   ---------- DHCP options ----------
   ----------------------------------
*/

/**
 * LWIP_DHCP==1: Enable DHCP module.
 */
#define LWIP_DHCP                       1

#define LWIP_DHCP_DOES_ACD_CHECK        0

/*
   ----------------------------------
   ---------- DNS options -----------
   ----------------------------------
*/

/**
 * LWIP_DNS==1: Turn on DNS module. UDP must be available for DNS
 * transport.
 */
#define LWIP_DNS                        1

#define LWIP_DNS_API_DECLARE_H_ERRNO    0
#define LWIP_DNS_API_DEFINE_ERRORS      0
#define LWIP_DNS_API_DEFINE_FLAGS       0
#define LWIP_DNS_API_DECLARE_STRUCTS    0

/*
   ---------------------------------
   ---------- UDP options ----------
   ---------------------------------
*/

/**
 * LWIP_UDP==1: Turn on UDP.
 */
#define LWIP_UDP                        1


/*
   ---------------------------------
   ---------- TCP options ----------
   ---------------------------------
*/

/**
 * LWIP_TCP==1: Turn on TCP.
 */
#define LWIP_TCP                        1

/**
 * TCP_MSS: TCP Maximum segment size.
 * For the receive side, this MSS is advertised to the remote side
 * when opening a connection. For the transmit size, this MSS sets
 * an upper limit on the MSS advertised by the remote host.
 *
 * Set 512 to match uclibc in <netinet/tcp.h>.
 */
#define TCP_MSS                         512

/*
   ------------------------------------------------
   ---------- Network Interfaces options ----------
   ------------------------------------------------
*/

/**
 * LWIP_NETIF_API==1: Support netif api (in netifapi.c)
 */
#define LWIP_NETIF_API                  1

/*
   ------------------------------------
   ---------- Socket options ----------
   ------------------------------------
*/

/**
 * LWIP_SOCKET==1: Enable Socket API (require to use sockets.c)
 */
#define LWIP_SOCKET                     1

/**
 * LWIP_COMPAT_SOCKETS==1: Enable BSD-style sockets functions names through defines.
 * LWIP_COMPAT_SOCKETS==2: Same as ==1 but correctly named functions are created.
 * While this helps code completion, it might conflict with existing libraries.
 * (only used if you use sockets.c)
 */
#define LWIP_COMPAT_SOCKETS             0

/**
 * LWIP_POSIX_SOCKETS_IO_NAMES==1: Enable POSIX-style sockets functions names.
 * Disable this option if you use a POSIX operating system that uses the same
 * names (read, write & close). (only used if you use sockets.c)
 */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0

/**
 * LWIP_SOCKET_EXTERNAL_HEADERS==1: Use external headers instead of sockets.h
 * and inet.h. In this case, user must provide its own headers by setting the
 * values for LWIP_SOCKET_EXTERNAL_HEADER_SOCKETS_H and
 * LWIP_SOCKET_EXTERNAL_HEADER_INET_H to appropriate include file names and the
 * whole content of the default sockets.h and inet.h is skipped.
 */
#define LWIP_SOCKET_EXTERNAL_HEADERS    1
#define LWIP_SOCKET_EXTERNAL_HEADER_SOCKETS_H  <lwipsockets.h>
#define LWIP_SOCKET_EXTERNAL_HEADER_INET_H     <lwipinet.h>

/**
 * LWIP_TCP_KEEPALIVE==1: Enable TCP_KEEPIDLE, TCP_KEEPINTVL and TCP_KEEPCNT
 * options processing. Note that TCP_KEEPIDLE and TCP_KEEPINTVL have to be set
 * in seconds. (does not require sockets.c, and will affect tcp.c)
 */
#define LWIP_TCP_KEEPALIVE              1

/**
 * LWIP_SO_SNDTIMEO==1: Enable send timeout for sockets/netconns and
 * SO_SNDTIMEO processing.
 */
#define LWIP_SO_SNDTIMEO                1

/**
 * LWIP_SO_RCVTIMEO==1: Enable receive timeout for sockets/netconns and
 * SO_RCVTIMEO processing.
 */
#define LWIP_SO_RCVTIMEO                1

/**
 * LWIP_SO_RCVBUF==1: Enable SO_RCVBUF processing.
 */
#define LWIP_SO_RCVBUF                  1

/**
 * LWIP_SO_LINGER==1: Enable SO_LINGER processing.
 */
#define LWIP_SO_LINGER                  1

/**
 * SO_REUSE==1: Enable SO_REUSEADDR option.
 */
#define SO_REUSE                        1

/*
   ---------------------------------------
   ---------- IPv6 options ---------------
   ---------------------------------------
*/

/**
 * LWIP_IPV6==1: Enable IPv6
 */
#define LWIP_IPV6                       1

/** IP6_FRAG_COPYHEADER==1: for platforms where sizeof(void*) > 4, "struct
 * ip6_reass_helper" is too large to be stored in the IPv6 fragment header, and
 * will bleed into the header before it, which may be the IPv6 header or an
 * extension header. This means that for each first fragment packet, we need to
 * 1) make a copy of some IPv6 header fields (src+dest) that we need later on,
 * just in case we do overwrite part of the IPv6 header, and 2) make a copy of
 * the header data that we overwrote, so that we can restore it before either
 * completing reassembly or sending an ICMPv6 reply. The last part is true even
 * if this setting is disabled, but if it is enabled, we need to save a bit
 * more data (up to the size of a pointer) because we overwrite more. */
#define IPV6_FRAG_COPYHEADER            1

/* Avoid clash with same definition in uclibc */
#define IF_NAMESIZE 16

/*
   ------------------------------------
   ----------- altcp / TLS ------------
   ------------------------------------
*/
/* Application-layered TCP + TLS via mbedTLS (pkg/mbedtls), unlocking mqtts /
 * https. The altcp_tls_mbedtls glue is compiled into liblwip (see lib/Makefile).
 * NOTE: the mbedTLS entropy backend is currently a WEAK software source — see
 * pkg/mbedtls/lib/entropy_l4.c; not for production until a real per-board source
 * (virtio-rng / RNGB) is wired in. */
#define LWIP_ALTCP                      1
#define LWIP_ALTCP_TLS                  1
#define LWIP_ALTCP_TLS_MBEDTLS          1

#ifdef __cplusplus
}
#endif
#endif /* LWIP_LWIPOPTS_H */
