PKGDIR       ?= ../..
L4DIR        ?= $(PKGDIR)/../..

TARGET        = ifconfig
SRC_CC        = main.cc
REQUIRES_LIBS = l4re-util libstdc++

# netd Net_svr protocol header.  ifconfig is a pure socket-over-IPC client: it
# links NO lwIP and holds NO sigma0 — it asks netd (which owns the stack) for
# the interface listing.  The netd cap is forwarded to it by spawnd.
PRIVATE_INCDIR += $(SRC_DIR)/../../../netd/include

include $(L4DIR)/mk/prog.mk
