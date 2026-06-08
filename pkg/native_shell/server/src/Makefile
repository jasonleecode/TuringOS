PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= native_shell
SRC_CC		= main.cc commands.cc cmd_net.cc cmd_ping.cc \
		  devices.cc dev_temp.cc dev_rtc.cc dev_radio.cc \
		  shell_exec.cc cmd_top.cc
REQUIRES_LIBS	= readline rtc rtc_libc_be ds18b20 tef6686hn \
		  lwip libc_be_socket_lwip libio-vbus libstdc++ l4util libsigma0 \
		  devfs libloader ext4_client libklog tmpfs

PRIVATE_INCDIR  += $(SRC_DIR)/../../../spawn/include \
                   $(SRC_DIR)/../../../filesystem/ext4/include

# Force linker to pull in the client TUs that self-register with the VFS:
# ext4_client registers its File_factory; tmpfs mounts /tmp.
LDFLAGS += -u ext4_client_module_init -u tmpfs_module_init

# Allow tcgetattr/tcsetattr in libreadline.a(replacements.o) to win over uclibc.
# L4Re vcon TCGETS/TCSETS ioctl corrupts the malloc heap; the readline replacements
# provide safe no-ops so all tty ioctl calls in native_shell become no-ops.


include $(L4DIR)/mk/prog.mk

# backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
# The build system strips _GNU_SOURCE (-U_GNU_SOURCE), so re-add it
# here (after the include) so GCC sees -U before -D and keeps it defined.
CPPFLAGS += -D_GNU_SOURCE
