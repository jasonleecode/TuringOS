PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= libnc_mqtt.a libnc_mqtt.so
SRC_CC		= mqtt.cc
PC_FILENAME	= nc_mqtt

REQUIRES_LIBS	= lwip

# mqtts pulls in lwip/altcp_tls.h, which includes <mbedtls/ssl.h>; give this TU
# the vendored mbedTLS headers + the L4Re config (pkg/mbedtls).  (lwIP links
# mbedtls transitively; only the *header* path isn't propagated by the .pc.)
PRIVATE_INCDIR	+= $(PKGDIR)/../mbedtls/lib/contrib/include \
		   $(PKGDIR)/../mbedtls/lib
DEFINES		+= -DMBEDTLS_CONFIG_FILE='<mbedtls_l4_config.h>'

include $(L4DIR)/mk/lib.mk

CPPFLAGS += -D_GNU_SOURCE
