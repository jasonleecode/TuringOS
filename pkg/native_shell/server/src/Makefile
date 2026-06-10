PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= native_shell
SRC_CC		= main.cc commands.cc cmd_net.cc cmd_ping.cc cmd_telnet.cc \
		  devices.cc dev_rtc.cc \
		  shell_exec.cc cmd_top.cc
# ds18b20 + tef6686hn libs dropped: the temp/radio drivers now run as their own
# servers (ds18b20-server / tef6686hn-server); the `temp`/`radio` commands are
# IPC clients (protocol headers only, no driver lib linked).
REQUIRES_LIBS	= readline rtc rtc_libc_be \
		  lwip libc_be_socket_lwip libio-vbus libstdc++ l4util libsigma0 \
		  devfs libloader ext4_client libklog tmpfs procfs \
		  nc_mqtt nc_telnet

PRIVATE_INCDIR  += $(SRC_DIR)/../../../spawn/include \
                   $(SRC_DIR)/../../../filesystem/ext4/include \
                   $(SRC_DIR)/../../../net-cluster/protocol/mqtt \
                   $(SRC_DIR)/../../../net-cluster/protocol/telnet \
                   $(SRC_DIR)/../../../ds18b20/include \
                   $(SRC_DIR)/../../../tef6686hn/include

# Force linker to pull in the client TUs that self-register with the VFS:
# ext4_client registers its File_factory; tmpfs mounts /tmp; procfs mounts
# /proc and /sys.
LDFLAGS += -u ext4_client_module_init -u tmpfs_module_init -u procfs_module_init

# Allow tcgetattr/tcsetattr in libreadline.a(replacements.o) to win over uclibc.
# L4Re vcon TCGETS/TCSETS ioctl corrupts the malloc heap; the readline replacements
# provide safe no-ops so all tty ioctl calls in native_shell become no-ops.


include $(L4DIR)/mk/prog.mk

# backend/vfs.h uses off64_t/stat64 which require __USE_LARGEFILE64.
# The build system strips _GNU_SOURCE (-U_GNU_SOURCE), so re-add it
# here (after the include) so GCC sees -U before -D and keeps it defined.
CPPFLAGS += -D_GNU_SOURCE
