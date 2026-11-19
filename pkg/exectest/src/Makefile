PKGDIR		?= ..
L4DIR		?= $(PKGDIR)/../..

TARGET		= exectest
SRC_CC		= main.cc

PRIVATE_INCDIR += $(SRC_DIR)/../../spawn/include

REQUIRES_LIBS	= libstdc++ l4re l4re_c l4util

REQUIRES_LIBS += ext4_client task_io
LDFLAGS += -u ext4_client_module_init -u task_io_init
include $(L4DIR)/mk/prog.mk
