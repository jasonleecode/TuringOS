PKGDIR       ?= ..
L4DIR        ?= $(PKGDIR)/../../..

TARGET        = authd
SRC_CC        = main.cc

# l4re-util: Registry_server framework.  mbedtls: salted SHA-256.  ext4_client:
# the VFS backend so fopen("/ext4/etc/shadow") resolves the credential file via
# the "ext4" namespace cap.
REQUIRES_LIBS = l4re-util libstdc++ mbedtls ext4_client

# Auth_svr protocol header + vendored mbedTLS headers/config (same incantation
# as net-cluster: the .pc doesn't propagate the header path or the config file).
PRIVATE_INCDIR += $(SRC_DIR)/../include \
                  $(SRC_DIR)/../../mbedtls/lib/contrib/include \
                  $(SRC_DIR)/../../mbedtls/lib
DEFINES        += -DMBEDTLS_CONFIG_FILE='<mbedtls_l4_config.h>'

# Pull in the ext4 VFS file factory (registers via static ctors) so the libc
# VFS can open files served by the ext4fs namespace.
LDFLAGS += -u ext4_client_module_init

include $(L4DIR)/mk/prog.mk

# l4re_vfs backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
CPPFLAGS += -D_GNU_SOURCE
