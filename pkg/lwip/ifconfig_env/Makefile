PKGDIR        ?= ..
L4DIR         ?= $(PKGDIR)/../../..

TARGET         = liblwip_ifconfig_env.a liblwip_ifconfig_env.so
LINK_INCR      = liblwip_ifconfig_env.a
PC_FILENAME    = lwip_ifconfig_env
SRC_CC         = lwip_ifconfig_env.cc
DEFINES        = -D_DEFAULT_SOURCE
REQUIRES_LIBS  = lwip libc_be_socket_lwip
DEPENDS_PKGS   = $(REQUIRES_LIBS)

include $(L4DIR)/mk/lib.mk

CFLAGS   += -fvisibility=hidden
CXXFLAGS += -fvisibility=hidden
