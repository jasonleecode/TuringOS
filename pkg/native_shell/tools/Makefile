PKGDIR ?= ..
L4DIR ?= $(PKGDIR)/../../..
TARGET = echo cat uname info clear svc app
$(foreach name,$(TARGET),$(eval SRC_CC_$(name) = $(name).cc))
SRC_CC_cat += ../server/src/devices.cc ../server/src/dev_rtc.cc
REQUIRES_LIBS = task_io ext4_client libstdc++ l4util procfs devfs rtc rtc_libc_be mbedtls
LDFLAGS += -u task_io_init
LDFLAGS_cat += -u ext4_client_module_init -u procfs_module_init
LDFLAGS_app += -u ext4_client_module_init
PRIVATE_INCDIR += $(SRC_DIR)/../../mbedtls/lib/contrib/include $(SRC_DIR)/../../mbedtls/lib
DEFINES += -DMBEDTLS_CONFIG_FILE='<mbedtls_l4_config.h>'
include $(L4DIR)/mk/prog.mk
# devfs uses the large-file types exposed by the GNU libc feature flags.
CPPFLAGS += -D_GNU_SOURCE
