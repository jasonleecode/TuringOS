# net-cluster — outward connectivity suite (remote access + IoT protocols).
#   protocol/ — small libraries linked into native_shell (legacy in-shell path).
#   tools/    — standalone net utilities that reach the network purely over
#               netd IPC (no lwIP / no sigma0); spawned via `run rom/<tool>`.
PKGDIR ?= .
L4DIR  ?= $(PKGDIR)/../..

TARGET = protocol tools

include $(L4DIR)/mk/subdir.mk
