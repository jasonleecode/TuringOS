PKGDIR          ?= ..
L4DIR           ?= $(PKGDIR)/../../..

TARGET	         = liblwip.a liblwip.so
LINK_INCR        = liblwip.a
PC_FILENAME      = lwip
REQUIRES_LIBS    = libpthread
CONTRIB_INCDIR   = lwip \
                   lwip/contrib/src/include \
                   lwip/contrib/contrib/ports/unix/port/include
DEFINES          = -DLWIP_L4=1 -D_DEFAULT_SOURCE

LWIPDIR          = contrib/src
include $(PKGDIR)/lib/$(LWIPDIR)/Filelists.mk

SRC_C            = $(filter-out contrib/src/netif/slipif.c \
                                contrib/src/core/ipv6/inet6.c,$(LWIPNOAPPSFILES)) \
                   contrib/contrib/ports/unix/port/perf.c \
                   contrib/contrib/ports/unix/port/sys_arch.c
SRC_CC           = l4libinit.cc

# ARCH-sparc/include/asm/socket.h is empty (missing SO_*) in uclibc
SYSTEMS        = $(filter-out sparc-l4f,$(SYSTEMS_ABI))

include $(L4DIR)/mk/lib.mk

$(GENERAL_D_LOC): $(PKGDIR)/lib/$(LWIPDIR)/Filelists.mk
