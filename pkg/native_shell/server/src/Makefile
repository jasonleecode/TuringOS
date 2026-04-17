PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= native_shell
SRC_CC		= main.cc commands.cc cmd_net.cc
REQUIRES_LIBS	= readline rtc rtc_libc_be ds18b20 tef6686hn \
		  lwip libc_be_socket_lwip libio-vbus libstdc++ l4util libsigma0

include $(L4DIR)/mk/prog.mk
