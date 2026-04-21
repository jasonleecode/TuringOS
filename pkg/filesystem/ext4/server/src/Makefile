PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= ext4fs
SRC_CC		= main.cc ext4_server.cc ext4_blockdev.cc

CONTRIB   := $(SRC_DIR)/../../contrib

PRIVATE_INCDIR := $(CONTRIB)/include

REQUIRES_LIBS = liblwext4 libio-vbus cxx_libc_io cxx_io libstdc++ l4util libsigma0

include $(L4DIR)/mk/prog.mk