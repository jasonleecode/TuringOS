PKGDIR       ?= ../..
L4DIR        ?= $(PKGDIR)/../..

TARGET        = telnetd
SRC_CC        = main.cc
REQUIRES_LIBS = l4re-util libstdc++ l4util

# netd Net_svr + spawnd Spawn_svr protocol headers.  telnetd holds no lwIP and
# no sigma0: it accepts a TCP connection via netd, presents it as an L4::Vcon,
# and asks spawnd (spawn_con) to launch native_shell with that vcon as console.

include $(L4DIR)/mk/prog.mk
