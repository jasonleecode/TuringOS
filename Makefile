# TuringOS Driver Configuration Makefile
#
# Usage:
#   make menuconfig   - Interactive driver configuration menu
#   make defconfig    - Apply default RPi4 configuration
#   make oldconfig    - Update .config with defaults for new symbols
#   make savedefconfig - Save current config as minimal defconfig
#   make clean        - Remove kconfig build artifacts

PROJ_ROOT := $(CURDIR)
KCONFIG_SRC := $(PROJ_ROOT)/l4mk/tool/kconfig/scripts/kconfig
KCONFIG_BUILD := $(PROJ_ROOT)/build/kconfig_tools
KCONFIG_FILE := $(PROJ_ROOT)/Kconfig

# All kconfig output goes under build/
KCONFIG_CONFIG := $(PROJ_ROOT)/build/.config
KCONFIG_AUTOHEADER := $(PROJ_ROOT)/build/include/generated/autoconf.h
KCONFIG_AUTOCONFIG := $(PROJ_ROOT)/build/include/config/auto.conf
KCONFIG_RUSTCCFG := $(PROJ_ROOT)/build/include/generated/rustc_cfg

export KCONFIG_CONFIG KCONFIG_AUTOHEADER KCONFIG_AUTOCONFIG KCONFIG_RUSTCCFG

HOSTCC ?= cc
HOSTCXX ?= c++
HOSTPKG_CONFIG ?= pkg-config

export HOSTCC HOSTCXX HOSTPKG_CONFIG

# Source files for kconfig common library
COMMON_SRCS := confdata.c expr.c menu.c preprocess.c symbol.c util.c
COMMON_OBJS := $(addprefix $(KCONFIG_BUILD)/, $(COMMON_SRCS:.c=.o))

# Generated parser/lexer
PARSER_OBJS := $(KCONFIG_BUILD)/parser.tab.o $(KCONFIG_BUILD)/lexer.lex.o

# mconf (menuconfig) objects
LXDIALOG_SRCS := checklist.c inputbox.c menubox.c textbox.c util.c yesno.c
LXDIALOG_OBJS := $(addprefix $(KCONFIG_BUILD)/lxdialog/, $(LXDIALOG_SRCS:.c=.o))
MCONF_OBJS := $(KCONFIG_BUILD)/mconf.o $(KCONFIG_BUILD)/mnconf-common.o \
              $(LXDIALOG_OBJS) $(COMMON_OBJS) $(PARSER_OBJS)

# conf (oldconfig/defconfig) objects
CONF_OBJS := $(KCONFIG_BUILD)/conf.o $(COMMON_OBJS) $(PARSER_OBJS)

# Detect ncurses flags
NCURSES_CFLAGS := $(shell $(HOSTPKG_CONFIG) --cflags ncursesw 2>/dev/null || \
                          $(HOSTPKG_CONFIG) --cflags ncurses 2>/dev/null || \
                          echo "-D_GNU_SOURCE")
NCURSES_LIBS := $(shell $(HOSTPKG_CONFIG) --libs ncursesw 2>/dev/null || \
                        $(HOSTPKG_CONFIG) --libs ncurses 2>/dev/null || \
                        echo "-lncurses")

HOST_CFLAGS := -I $(KCONFIG_SRC) -I $(PROJ_ROOT)/l4mk/tool/kconfig/scripts/include
HOST_CFLAGS_MCONF := $(NCURSES_CFLAGS)

# ============================================================
# Targets
# ============================================================

.PHONY: menuconfig defconfig oldconfig olddefconfig savedefconfig \
        allnoconfig allyesconfig syncconfig clean help

menuconfig: $(KCONFIG_BUILD)/mconf | build_dirs
	$< $(KCONFIG_FILE)
	@$(MAKE) --no-print-directory syncconfig

defconfig: $(KCONFIG_BUILD)/conf | build_dirs
	$< --defconfig=$(PROJ_ROOT)/defconfig $(KCONFIG_FILE)
	@$(MAKE) --no-print-directory syncconfig

oldconfig: $(KCONFIG_BUILD)/conf | build_dirs
	$< --oldconfig $(KCONFIG_FILE)
	@$(MAKE) --no-print-directory syncconfig

olddefconfig: $(KCONFIG_BUILD)/conf | build_dirs
	$< --olddefconfig $(KCONFIG_FILE)
	@$(MAKE) --no-print-directory syncconfig

savedefconfig: $(KCONFIG_BUILD)/conf | build_dirs
	$< --savedefconfig=defconfig $(KCONFIG_FILE)

allnoconfig: $(KCONFIG_BUILD)/conf | build_dirs
	$< --allnoconfig $(KCONFIG_FILE)
	@$(MAKE) --no-print-directory syncconfig

allyesconfig: $(KCONFIG_BUILD)/conf | build_dirs
	$< --allyesconfig $(KCONFIG_FILE)
	@$(MAKE) --no-print-directory syncconfig

# Generate build/include/config/auto.conf and build/include/generated/autoconf.h
syncconfig: $(KCONFIG_BUILD)/conf | build_dirs
	@test -f $(KCONFIG_CONFIG) || \
		{ echo "Error: .config not found. Run 'make defconfig' or 'make menuconfig' first."; exit 1; }
	$< --syncconfig $(KCONFIG_FILE)

help:
	@echo 'TuringOS Driver Configuration'
	@echo ''
	@echo 'Configuration targets:'
	@echo '  menuconfig    - Interactive ncurses menu'
	@echo '  defconfig     - Apply default RPi4 configuration'
	@echo '  oldconfig     - Update config, prompting for new symbols'
	@echo '  olddefconfig  - Update config, using defaults for new symbols'
	@echo '  savedefconfig - Save current .config as minimal defconfig'
	@echo '  syncconfig    - Regenerate auto.conf and autoconf.h from .config'
	@echo '  allnoconfig   - Disable all optional drivers'
	@echo '  allyesconfig  - Enable all optional drivers'
	@echo '  clean         - Remove kconfig build artifacts'
	@echo ''
	@echo 'Output files (all under build/):'
	@echo '  build/.config                       - Current driver configuration'
	@echo '  build/include/config/auto.conf      - Makefile-includable config'
	@echo '  build/include/generated/autoconf.h  - C header with CONFIG_* defines'

# ============================================================
# Build kconfig tools
# ============================================================

# Ensure output directories exist
.PHONY: build_dirs
build_dirs:
	@mkdir -p $(dir $(KCONFIG_AUTOHEADER))
	@mkdir -p $(dir $(KCONFIG_AUTOCONFIG))

$(KCONFIG_BUILD)/mconf: $(MCONF_OBJS)
	$(HOSTCC) -o $@ $^ $(NCURSES_LIBS)

$(KCONFIG_BUILD)/conf: $(CONF_OBJS)
	$(HOSTCC) -o $@ $^

# Generate parser and lexer from grammar files
$(KCONFIG_BUILD)/parser.tab.c $(KCONFIG_BUILD)/parser.tab.h: $(KCONFIG_SRC)/parser.y | $(KCONFIG_BUILD)
	bison -o $(KCONFIG_BUILD)/parser.tab.c --defines=$(KCONFIG_BUILD)/parser.tab.h $<

$(KCONFIG_BUILD)/lexer.lex.c: $(KCONFIG_SRC)/lexer.l $(KCONFIG_BUILD)/parser.tab.h | $(KCONFIG_BUILD)
	flex -o $@ $<

# Compile generated parser/lexer
$(KCONFIG_BUILD)/parser.tab.o: $(KCONFIG_BUILD)/parser.tab.c
	$(HOSTCC) $(HOST_CFLAGS) -I $(KCONFIG_BUILD) -DYYDEBUG -c -o $@ $<

$(KCONFIG_BUILD)/lexer.lex.o: $(KCONFIG_BUILD)/lexer.lex.c
	$(HOSTCC) $(HOST_CFLAGS) -I $(KCONFIG_BUILD) -c -o $@ $<

# Compile kconfig sources (ncurses flags are harmless for non-mconf objects)
$(KCONFIG_BUILD)/%.o: $(KCONFIG_SRC)/%.c | $(KCONFIG_BUILD)
	$(HOSTCC) $(HOST_CFLAGS) $(HOST_CFLAGS_MCONF) -c -o $@ $<

$(KCONFIG_BUILD)/lxdialog/%.o: $(KCONFIG_SRC)/lxdialog/%.c | $(KCONFIG_BUILD)
	$(HOSTCC) $(HOST_CFLAGS) $(HOST_CFLAGS_MCONF) -c -o $@ $<

# Create build directories
$(KCONFIG_BUILD):
	mkdir -p $@/lxdialog

# ============================================================
# Clean
# ============================================================

clean:
	rm -rf $(KCONFIG_BUILD)
	rm -f $(KCONFIG_CONFIG) $(KCONFIG_CONFIG).old
	rm -rf $(PROJ_ROOT)/build/include
