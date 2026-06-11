PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = devmgr
SRC_CC        = main.cc
REQUIRES_LIBS = l4util libstdc++

# spawn_ipc.h (the Spawn_svr protocol used to launch drivers) + the shared
# device model (device_table.h).
PRIVATE_INCDIR += $(SRC_DIR)/../../spawn/include $(SRC_DIR)/../include

include $(L4DIR)/mk/prog.mk
