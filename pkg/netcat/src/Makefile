PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = netcat
SRC_CC        = main.cc
REQUIRES_LIBS = l4re-util libstdc++

# The netd Net_svr protocol header (net_ipc.h).  netcat is a pure socket-over-
# IPC client: it links NO lwIP and holds NO sigma0 — the network lives entirely
# in netd, reached through the forwarded "netd" cap.
PRIVATE_INCDIR += $(SRC_DIR)/../../netd/include

include $(L4DIR)/mk/prog.mk
