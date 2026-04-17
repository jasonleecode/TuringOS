PKGDIR       ?= ../..
# l4mk/ is 4 levels up from pkg/hello/tcp_server/src/
# (pkg/hello -> pkg -> TuringOS -> l4mk)
L4DIR        ?= $(PKGDIR)/../../l4mk

TARGET        = tcp_server
SRC_CC        = main.cc
REQUIRES_LIBS = lwip libc_be_socket_lwip libio-vbus libstdc++ l4util libsigma0

include $(L4DIR)/mk/prog.mk
