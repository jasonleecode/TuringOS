PKGDIR ?= ..
L4DIR  ?= $(PKGDIR)/../../../l4mk

TARGET = liblvgl.a

# lv_conf.h is in SRC_DIR — tell LVGL to find it by simple include
DEFINES += -DLV_CONF_INCLUDE_SIMPLE

PRIVATE_INCDIR += $(SRC_DIR) $(SRC_DIR)/lvgl $(SRC_DIR)/lvgl/src

# Collect all LVGL core + widgets demo source files
SRC_C = $(patsubst $(SRC_DIR)/%,%, \
    $(shell find $(SRC_DIR)/lvgl/src -name '*.c') \
    $(shell find $(SRC_DIR)/lvgl/demos/widgets -name '*.c'))

# Suppress warnings from upstream code
CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-misleading-indentation \
          -Wno-maybe-uninitialized -Wno-implicit-fallthrough

include $(L4DIR)/mk/lib.mk
