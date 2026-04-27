PKGDIR		?= ../../..
L4DIR		?= $(PKGDIR)/../../..

TARGET		= libext4_client.a libext4_client.so

SRC_CC		= ext4_vfs.cc

PC_FILENAME	= ext4_client

include $(L4DIR)/mk/lib.mk

# backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
# The build system strips _GNU_SOURCE before the include, so re-add it after.
CPPFLAGS += -D_GNU_SOURCE
