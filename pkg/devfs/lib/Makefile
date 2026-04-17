PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = libdevfs.a libdevfs.so
SRC_CC        = devfs.cc
PC_FILENAME   = devfs
REQUIRES_LIBS = libstdc++

include $(L4DIR)/mk/lib.mk

CPPFLAGS += -D_GNU_SOURCE
