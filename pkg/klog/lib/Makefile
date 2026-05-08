PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = libklog.a libklog.so
SRC_CC        = klog.cc
PC_FILENAME   = libklog
REQUIRES_LIBS = libstdc++ l4re

include $(L4DIR)/mk/lib.mk

CPPFLAGS += -D_GNU_SOURCE
