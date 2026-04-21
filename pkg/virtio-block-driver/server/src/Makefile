PKGDIR ?= ..
L4DIR ?= $(PKGDIR)/../..

TARGET = virtio-block-driver
SRC_CC = main.cc virtio_device.cc

REQUIRES_LIBS = libblock-device libio-vbus stdlibs l4virtio libstdc++

include $(L4DIR)/mk/prog.mk