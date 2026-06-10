PKGDIR		?= ..
L4DIR		?= $(PKGDIR)/../..

TARGET		= libmbedtls.a libmbedtls.so
PC_FILENAME	= mbedtls
REQUIRES_LIBS	= l4sys l4re l4util

# Export both the L4Re config (lib/ root) and the vendored mbedTLS/PSA headers
# (contrib/include) to dependents (e.g. lwIP's altcp_tls glue):
#   <mbedtls_l4_config.h>  via  include/contrib/mbedtls
#   <mbedtls/ssl.h> etc.   via  include/contrib/mbedtls/contrib/include
CONTRIB_INCDIR	= mbedtls mbedtls/contrib/include

# All vendored library sources except net_sockets.c (altcp provides the BIO).
# Relative-to-SRC_DIR so the build's VPATH resolves them.
MBEDTLS_SRCS	:= $(patsubst $(SRC_DIR)/%,%,$(wildcard $(SRC_DIR)/contrib/library/*.c))
SRC_C		= entropy_l4.c \
		  $(filter-out contrib/library/net_sockets.c,$(MBEDTLS_SRCS))

PRIVATE_INCDIR	= $(SRC_DIR) $(SRC_DIR)/contrib/include
DEFINES		+= -DMBEDTLS_CONFIG_FILE='<mbedtls_l4_config.h>'

# ARCH-sparc lacks some bits; mirror lwip's exclusion.
SYSTEMS		= $(filter-out sparc-l4f,$(SYSTEMS_ABI))

# Vendored third-party C — don't fail on its warning hygiene.
WARNINGS	=
CFLAGS		+= -Wno-error -Wno-unused -Wno-redundant-decls -Wno-format

include $(L4DIR)/mk/lib.mk
