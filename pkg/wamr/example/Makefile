PKGDIR        ?= ..
L4DIR         ?= $(PKGDIR)/../../..

TARGET         = wamr-example
SRC_C          = main.c
REQUIRES_LIBS  = wamr libc l4re

WAMR_ROOT     := $(PKGDIR)/core

PRIVATE_INCDIR = \
	$(WAMR_ROOT)/shared/platform/include  \
	$(WAMR_ROOT)/shared/platform/l4re    \
	$(WAMR_ROOT)/iwasm/include

include $(L4DIR)/mk/prog.mk
