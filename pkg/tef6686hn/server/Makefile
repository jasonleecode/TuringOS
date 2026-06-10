PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = tef6686hn-server
SRC_CC        = main.cc
# tef6686hn lib + i2c-server (I2c_device_ops) for the driver; l4re-util for
# the server framework (Registry_server / Br_manager / object_registry).
REQUIRES_LIBS = tef6686hn i2c-server l4re-util l4util libstdc++

include $(L4DIR)/mk/prog.mk
