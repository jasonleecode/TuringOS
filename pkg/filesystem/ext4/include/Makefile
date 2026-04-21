PKGDIR  ?= ..
L4DIR   ?= $(PKGDIR)/../..

include $(L4DIR)/mk/Makeconf

# Resolve the real path of contrib/include, bypassing symlink ambiguity
CONTRIB_INC := $(shell readlink -f $(PKGDIR_ABS)/contrib/include)
INSTALL_DIR := $(OBJ_BASE)/include/contrib/ext4

HEADERS := ext4.h ext4_blockdev.h ext4_errno.h ext4_oflags.h \
           ext4_config.h ext4_types.h ext4_mbr.h ext4_mkfs.h \
           ext4_debug.h ext4_misc.h

all::
	$(VERBOSE)mkdir -p $(INSTALL_DIR)
	$(VERBOSE)$(foreach h,$(HEADERS), \
	  test -e $(INSTALL_DIR)/$(h) || \
	  ln -sf $(CONTRIB_INC)/$(h) $(INSTALL_DIR)/$(h);)

install:: all
