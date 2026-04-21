PKGDIR    ?= ../..
L4DIR     ?= $(PKGDIR)/../..

CONTRIB   := $(SRC_DIR)/../../contrib

TARGET    := liblwext4.a liblwext4.so
PC_FILENAME := liblwext4
SYSTEMS     := $(SYSTEMS_ABI)

SRC_C := ext4_balloc.c  \
         ext4_bcache.c  \
         ext4_bitmap.c  \
         ext4_blockdev.c \
         ext4_block_group.c \
         ext4.c         \
         ext4_crc32.c   \
         ext4_debug.c   \
         ext4_dir.c     \
         ext4_dir_idx.c \
         ext4_extent.c  \
         ext4_fs.c      \
         ext4_hash.c    \
         ext4_ialloc.c  \
         ext4_inode.c   \
         ext4_journal.c \
         ext4_mbr.c     \
         ext4_mkfs.c    \
         ext4_super.c   \
         ext4_trans.c   \
         ext4_xattr.c

PRIVATE_INCDIR := $(CONTRIB)/include

vpath %.c $(CONTRIB)/src

# Use defaults from ext4_config.h; no cmake-generated config needed
CPPFLAGS += -DCONFIG_USE_DEFAULT_CFG=1

# Suppress warnings from upstream code
CFLAGS += -Wno-unused-parameter -Wno-sign-compare

include $(L4DIR)/mk/lib.mk
