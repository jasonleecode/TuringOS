PKGDIR ?= ..
L4DIR  ?= $(PKGDIR)/../../../l4mk
TARGET = liblvgl-port.a
PC_FILENAME = lvgl-port
SRC_CC = runtime.cc lv_port_disp.cc lv_port_input.cc
PRIVATE_INCDIR += $(SRC_DIR)/../../liblvgl/src
REQUIRES_LIBS = liblvgl libstdc++ l4re l4re_c l4re-util l4util libio-vbus libklog ext4_client
include $(L4DIR)/mk/lib.mk
