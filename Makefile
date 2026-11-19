# Configure the same board build tree that produces the image.
.DEFAULT_GOAL := all
BOARD ?= rpi4
# Recursive GNU make keeps one shared pool of jobs across the board build.
build_mode_flags = $(filter-out -%,$(firstword $(MAKEFLAGS)))
skip_execution = $(or $(findstring n,$(build_mode_flags)),$(findstring q,$(build_mode_flags)),$(findstring t,$(build_mode_flags)))
make_jobs = $(patsubst -j%,%,$(filter -j%,$(MAKEFLAGS)))
use_jobserver = $(if $(filter-out $(make_jobs),$(JOBS)),,$(filter --jobserver-%,$(MAKEFLAGS)))
build_jobs = $(or $(JOBS),$(make_jobs))
ifneq ($(strip $(O)),)
export BUILD_OUT := $(abspath $(O))
endif
# O and ENTRY have their own meanings in the underlying kernel and L4Re makes.
MAKEOVERRIDES :=
unexport O
CONFIG_TARGETS := menuconfig defconfig oldconfig olddefconfig syncconfig savedefconfig allnoconfig allyesconfig
BUILD_TARGETS := all kernel l4re bootstrap collect clean cleanall
.PHONY: $(CONFIG_TARGETS) $(BUILD_TARGETS) help
$(CONFIG_TARGETS) $(BUILD_TARGETS):
	$(if $(skip_execution),,+)@TURINGOS_JOBSERVER=$(if $(use_jobserver),1,0) JOBS="$(build_jobs)" ./build.sh --board "$(BOARD)" $@
help:
	@./build.sh --help
