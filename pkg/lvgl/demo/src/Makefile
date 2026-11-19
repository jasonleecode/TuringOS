PKGDIR ?= ..
L4DIR  ?= $(PKGDIR)/../../../l4mk

TARGET  = lvgl-demo
SRC_CC  = main.cc

PRIVATE_INCDIR += $(SRC_DIR) $(SRC_DIR)/../../liblvgl/src

REQUIRES_LIBS = lvgl-port

# Pull in the ext4_client TU that registers the Ext4_file_ops VFS factory.
LDFLAGS += -u ext4_client_module_init

include $(L4DIR)/mk/prog.mk
