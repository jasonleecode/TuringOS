PKGDIR ?= ..
L4DIR  ?= $(PKGDIR)/../../../l4mk

TARGET        = smp-spawn-bench
SRC_CC        = smp_spawn_bench.cc
PRIVATE_INCDIR += $(SRC_DIR)/../../../spawn/include
REQUIRES_LIBS = libstdc++ l4re l4re_c l4util libpthread

include $(L4DIR)/mk/prog.mk
