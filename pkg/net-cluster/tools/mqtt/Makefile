PKGDIR       ?= ../..
L4DIR        ?= $(PKGDIR)/../..

TARGET        = mqtt
SRC_CC        = main.cc
REQUIRES_LIBS = l4re-util libstdc++ l4util

PRIVATE_INCDIR += $(SRC_DIR)/../../../netd/include

include $(L4DIR)/mk/prog.mk
