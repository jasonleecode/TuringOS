PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

TARGET        = syslogd
SRC_CC        = main.cc

PRIVATE_INCDIR += $(SRC_DIR)/../../../klog/include

REQUIRES_LIBS = libstdc++ l4re-util l4util ext4_client

LDFLAGS += -u ext4_client_module_init

include $(L4DIR)/mk/prog.mk

CPPFLAGS += -D_GNU_SOURCE
