PKGDIR ?= .
L4DIR ?= $(PKGDIR)/../../..

TARGET = include lib libc_be_socket virtio_net ifconfig_env

include $(L4DIR)/mk/subdir.mk

ifconfig_env: lib libc_be_socket
virtio_net: lib
libc_be_socket: lib
lib: include
