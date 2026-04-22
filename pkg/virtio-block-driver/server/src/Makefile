PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

TARGET    = virtio-block-driver
SRC_CC    = main.cc virtio_device.cc

REQUIRES_LIBS = libio-vbus libblock-device

include $(L4DIR)/mk/prog.mk
