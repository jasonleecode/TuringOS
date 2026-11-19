PKGDIR ?= ..
L4DIR ?= $(PKGDIR)/../../..
TARGET = wasm-run
SRC_C = main.c
REQUIRES_LIBS = wamr ext4_client libstdc++ task_io
PRIVATE_INCDIR = $(PKGDIR)/core/iwasm/include $(PKGDIR)/core/shared/platform/include $(PKGDIR)/core/shared/platform/l4re
LDFLAGS += -u ext4_client_module_init -u task_io_init
include $(L4DIR)/mk/prog.mk
