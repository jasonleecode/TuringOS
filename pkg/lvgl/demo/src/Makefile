PKGDIR ?= ..
L4DIR  ?= $(PKGDIR)/../../../l4mk

TARGET  = lvgl-demo
SRC_CC  = main.cc lv_port_disp.cc lv_port_input.cc

PRIVATE_INCDIR += $(SRC_DIR) $(SRC_DIR)/../../liblvgl/src

REQUIRES_LIBS = liblvgl libstdc++ l4re l4re_c l4re-util l4util libio-vbus libsigma0

include $(L4DIR)/mk/prog.mk
