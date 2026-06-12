PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= native_shell
SRC_CC		= main.cc commands.cc \
		  devices.cc dev_rtc.cc \
		  shell_exec.cc cmd_top.cc
# The network stack is gone from native_shell: it lives in the standalone netd
# server, and all net commands are separate tools (pkg/net-cluster/tools).  So
# the shell no longer links lwIP / the libc socket backend / sigma0 / the
# net-cluster protocol libs — it holds no network code and no raw-memory cap.
# (ds18b20/tef6686hn libs were dropped earlier: temp/radio are IPC clients.)
REQUIRES_LIBS	= readline rtc rtc_libc_be \
		  libio-vbus libstdc++ l4util \
		  devfs libloader ext4_client libklog tmpfs procfs

PRIVATE_INCDIR  += $(SRC_DIR)/../../../spawn/include \
                   $(SRC_DIR)/../../../filesystem/ext4/include \
                   $(SRC_DIR)/../../../ds18b20/include \
                   $(SRC_DIR)/../../../tef6686hn/include \
                   $(SRC_DIR)/../../../authd/include

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
