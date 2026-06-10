PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = ds18b20-server
SRC_CC        = main.cc
# ds18b20 lib + vbus for the driver; l4re-util for the server framework
# (Registry_server / Br_manager / object_registry).
REQUIRES_LIBS = ds18b20 l4re-util libio-vbus l4util libstdc++

include $(L4DIR)/mk/prog.mk
