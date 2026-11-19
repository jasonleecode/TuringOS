PKGDIR ?= ..
L4DIR  ?= $(PKGDIR)/../../../l4mk

TARGET        = ipcbench
SRC_CC        = ipcbench.cc
REQUIRES_LIBS = libstdc++ l4re l4re_c l4util libpthread

REQUIRES_LIBS += task_io
LDFLAGS += -u task_io_init
include $(L4DIR)/mk/prog.mk
