PKGDIR ?= ..
L4DIR ?= $(PKGDIR)/../../..
TARGET = libtask_io.a
PC_FILENAME = task_io
SRC_CC = pipe.cc socket.cc
REQUIRES_LIBS = libstdc++ l4util l4re-util
include $(L4DIR)/mk/lib.mk
CPPFLAGS += -D_GNU_SOURCE
