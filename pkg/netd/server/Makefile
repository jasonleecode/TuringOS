PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = netd
SRC_CC        = main.cc

# lwip + its libc socket backend (the TCP/IP stack runs in-process here);
# libio-vbus for the NIC MMIO (via vbus, not sigma0); l4re-util for the
# Registry_server framework.
REQUIRES_LIBS = lwip libc_be_socket_lwip libio-vbus l4re-util l4util libstdc++

# The Net_svr protocol header (net_ipc.h) lives in this package's include dir.
PRIVATE_INCDIR += $(SRC_DIR)/../include

include $(L4DIR)/mk/prog.mk

# lwip's backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
CPPFLAGS += -D_GNU_SOURCE
