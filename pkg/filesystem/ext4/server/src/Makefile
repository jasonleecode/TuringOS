PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= ext4fs
SRC_CC		= main.cc virtio_blockdev.cc ext4_ns.cc

CONTRIB   := $(SRC_DIR)/../../contrib

PRIVATE_INCDIR := $(CONTRIB)/include

# Use ext4_config.h defaults; no cmake-generated config needed
CPPFLAGS += -DCONFIG_USE_DEFAULT_CFG=1

REQUIRES_LIBS = liblwext4 l4virtio libio-vbus cxx_libc_io cxx_io libstdc++ l4util libsigma0

include $(L4DIR)/mk/prog.mk
