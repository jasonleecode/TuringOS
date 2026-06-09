PKGDIR		?= ..
L4DIR		?= $(PKGDIR)/../..

TARGET		= libprocfs.a libprocfs.so

SRC_CC		= procfs.cc

PC_FILENAME	= procfs

PRIVATE_INCDIR += $(SRC_DIR)/../../spawn/include

include $(L4DIR)/mk/lib.mk

# backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
CPPFLAGS += -D_GNU_SOURCE
