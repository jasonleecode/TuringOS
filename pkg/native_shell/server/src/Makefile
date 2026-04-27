PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= native_shell
SRC_CC		= main.cc commands.cc cmd_net.cc \
		  devices.cc dev_temp.cc dev_rtc.cc dev_radio.cc \
		  shell_exec.cc log.cc
REQUIRES_LIBS	= readline rtc rtc_libc_be ds18b20 tef6686hn \
		  lwip libc_be_socket_lwip libio-vbus libstdc++ l4util libsigma0 \
		  devfs libloader ext4_client

# Force linker to pull in the ext4_client TU that registers the File_factory.
LDFLAGS += -u ext4_client_module_init

include $(L4DIR)/mk/prog.mk

# backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
# The build system strips _GNU_SOURCE (-U_GNU_SOURCE), so re-add it
# here (after the include) so GCC sees -U before -D and keeps it defined.
CPPFLAGS += -D_GNU_SOURCE
