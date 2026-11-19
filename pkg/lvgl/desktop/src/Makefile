PKGDIR ?= ..
L4DIR  ?= $(PKGDIR)/../../../l4mk
TARGET = lvgl-desktop
SRC_CC = desktop.cc icons.cc minesweeper.cc
SRC_C = desktop_font.c
SRC_DATA = meadow.rgb565
PRIVATE_INCDIR += $(SRC_DIR)/../../liblvgl/src
REQUIRES_LIBS = lvgl-port rtc_libc_be
LDFLAGS += -u ext4_client_module_init
REQUIRES_LIBS += task_io
LDFLAGS += -u task_io_init
include $(L4DIR)/mk/prog.mk
