PKGDIR       ?= ../..
L4DIR        ?= $(PKGDIR)/../..

TARGET        = ping
SRC_CC        = main.cc
REQUIRES_LIBS = l4re-util libstdc++ l4util


REQUIRES_LIBS += task_io
LDFLAGS += -u task_io_init
include $(L4DIR)/mk/prog.mk
