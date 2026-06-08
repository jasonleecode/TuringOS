PKGDIR		?= ..
L4DIR		?= $(PKGDIR)/../..

TARGET		= libtmpfs.a libtmpfs.so

SRC_CC		= tmpfs.cc

PC_FILENAME	= tmpfs

include $(L4DIR)/mk/lib.mk

# backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
# The build system strips _GNU_SOURCE before the include, so re-add it after.
CPPFLAGS += -D_GNU_SOURCE
