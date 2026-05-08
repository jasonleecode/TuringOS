PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

TARGET  = spawnd
SRC_CC  = main.cc spawnd.cc app_model.cc task_table.cc

PRIVATE_INCDIR += $(SRC_DIR)/../../../spawn/include \
                  $(SRC_DIR)/../../../klog/include

REQUIRES_LIBS = libstdc++ l4re-util l4util libloader ext4_client libklog

LDFLAGS += -u ext4_client_module_init

include $(L4DIR)/mk/prog.mk

CPPFLAGS += -D_GNU_SOURCE
