PKGDIR        ?= ..
L4DIR         ?= $(PKGDIR)/../../..

TARGET         = libc_be_socket_lwip.a libc_be_socket_lwip.so
PC_FILENAME    = libc_be_socket_lwip
SRC_CC         = socket.cc
SRC_C          = ifaddrs.c
DEFINES        = -D_GNU_SOURCE
REQUIRES_LIBS  = lwip
DEPENDS_PKGS   = $(REQUIRES_LIBS)

include $(L4DIR)/mk/lib.mk
