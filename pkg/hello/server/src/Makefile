PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../../../l4mk

TARGET		= hello
SRC_C		= main.c

REQUIRES_LIBS += task_io
LDFLAGS += -u task_io_init
include $(L4DIR)/mk/prog.mk
