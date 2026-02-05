PKGDIR        ?= ..
L4DIR         ?= $(PKGDIR)/../../..

TARGET         = liblwip_virtio_net.a liblwip_virtio_net.so
LINK_INCR      = liblwip_virtio_net.a
PC_FILENAME    = lwip_virtio_net
SRC_CC         = lwip_virtio_net.cc
REQUIRES_LIBS  = lwip l4virtio libstdc++
DEPENDS_PKGS   = $(REQUIRES_LIBS)

include $(L4DIR)/mk/lib.mk

CFLAGS   += -fvisibility=hidden
CXXFLAGS += -fvisibility=hidden
