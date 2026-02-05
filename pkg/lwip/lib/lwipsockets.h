/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2022 Kernkonzept GmbH.
 * Author(s): Stephan Gerhold <stephan.gerhold@kernkonzept.com>
 */

#ifndef LWIP_HDR_EXTERNAL_SOCKETS_H
#define LWIP_HDR_EXTERNAL_SOCKETS_H

#include <asm/ioctls.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>

typedef size_t msg_iovlen_t;

/*
 * Extra socket option not supported by uclibc,
 * just define it to an unused magic number.
 */
#define TCP_KEEPALIVE 42

#define SIN_ZERO_LEN         sizeof(((struct sockaddr_in *)0)->sin_zero)
#define LWIP_SELECT_MAXNFDS  FD_SETSIZE

#endif /* LWIP_HDR_EXTERNAL_SOCKETS_H */
