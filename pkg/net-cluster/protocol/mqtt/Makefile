PKGDIR		?= ../..
L4DIR		?= $(PKGDIR)/../..

TARGET		= libnc_mqtt.a libnc_mqtt.so
SRC_CC		= mqtt.cc
PC_FILENAME	= nc_mqtt

REQUIRES_LIBS	= lwip

include $(L4DIR)/mk/lib.mk

CPPFLAGS += -D_GNU_SOURCE
