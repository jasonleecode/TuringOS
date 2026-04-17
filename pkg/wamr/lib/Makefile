PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = libwamr.a libwamr.so
PC_FILENAME   = wamr
REQUIRES_LIBS = libc l4re

# ---- Feature flags --------------------------------------------------------
#
#  Interpreter-only, classic mode, single-threaded, libc-builtin, no WASI.
#  AOT sources are present in core/ but not compiled; enable by adding them
#  to SRC_C and flipping WASM_ENABLE_AOT=1 when wamrc-compiled binaries
#  are needed.
#
DEFINES += \
	-DWASM_ENABLE_INTERP=1          \
	-DWASM_ENABLE_FAST_INTERP=0     \
	-DWASM_ENABLE_AOT=0             \
	-DWASM_ENABLE_JIT=0             \
	-DWASM_ENABLE_FAST_JIT=0        \
	-DWASM_ENABLE_GC=0              \
	-DWASM_ENABLE_SHARED_MEMORY=0   \
	-DWASM_ENABLE_LIBC_BUILTIN=1    \
	-DWASM_ENABLE_LIBC_WASI=0       \
	-DWASM_ENABLE_MULTI_MODULE=0    \
	-DWASM_ENABLE_THREAD_MGR=0      \
	-DWASM_ENABLE_SIMD=0            \
	-DWASM_ENABLE_BULK_MEMORY=1     \
	-DWASM_DISABLE_HW_BOUND_CHECK=1 \
	-DWASM_DISABLE_STACK_HW_BOUND_CHECK=1 \
	-DBH_MALLOC=wasm_runtime_malloc \
	-DBH_FREE=wasm_runtime_free

# ---- Include directories --------------------------------------------------

WAMR_ROOT    := $(PKGDIR)/core

PRIVATE_INCDIR = \
	$(WAMR_ROOT)                                    \
	$(WAMR_ROOT)/shared/platform/include            \
	$(WAMR_ROOT)/shared/platform/l4re              \
	$(WAMR_ROOT)/shared/mem-alloc                  \
	$(WAMR_ROOT)/shared/mem-alloc/ems              \
	$(WAMR_ROOT)/shared/utils                      \
	$(WAMR_ROOT)/iwasm/include                     \
	$(WAMR_ROOT)/iwasm/common                      \
	$(WAMR_ROOT)/iwasm/interpreter                 \
	$(WAMR_ROOT)/iwasm/libraries/libc-builtin

# ---- Source files ---------------------------------------------------------

# Platform abstraction layer
SRC_C += ../core/shared/platform/l4re/l4re_platform.c

# Memory allocator (EMS)
SRC_C += \
	../core/shared/mem-alloc/mem_alloc.c            \
	../core/shared/mem-alloc/ems/ems_alloc.c        \
	../core/shared/mem-alloc/ems/ems_gc.c           \
	../core/shared/mem-alloc/ems/ems_hmu.c          \
	../core/shared/mem-alloc/ems/ems_kfc.c

# Utility library
SRC_C += \
	../core/shared/utils/bh_assert.c               \
	../core/shared/utils/bh_bitmap.c               \
	../core/shared/utils/bh_common.c               \
	../core/shared/utils/bh_hashmap.c              \
	../core/shared/utils/bh_leb128.c               \
	../core/shared/utils/bh_list.c                 \
	../core/shared/utils/bh_log.c                  \
	../core/shared/utils/bh_queue.c                \
	../core/shared/utils/bh_vector.c

# WASM runtime common layer
SRC_C += \
	../core/iwasm/common/wasm_application.c        \
	../core/iwasm/common/wasm_c_api.c              \
	../core/iwasm/common/wasm_exec_env.c           \
	../core/iwasm/common/wasm_loader_common.c      \
	../core/iwasm/common/wasm_memory.c             \
	../core/iwasm/common/wasm_native.c             \
	../core/iwasm/common/wasm_runtime_common.c     \
	../core/iwasm/common/arch/invokeNative_general.c

# Classic interpreter
SRC_C += \
	../core/iwasm/interpreter/wasm_interp_classic.c \
	../core/iwasm/interpreter/wasm_loader.c         \
	../core/iwasm/interpreter/wasm_runtime.c

# Built-in libc wrappers (printf, memcpy, etc. exposed to WASM)
SRC_C += ../core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c

include $(L4DIR)/mk/lib.mk
