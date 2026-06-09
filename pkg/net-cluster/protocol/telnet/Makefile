PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= libnc_telnet.a libnc_telnet.so
SRC_CC		= telnetd.cc
PC_FILENAME	= nc_telnet

REQUIRES_LIBS	= lwip libc_be_socket_lwip

include $(L4DIR)/mk/lib.mk

CPPFLAGS += -D_GNU_SOURCE
