# Include before prog.mk. The matching Control dependencies are task_io and ext4_client.
REQUIRES_LIBS += task_io ext4_client
LDFLAGS += -u task_io_init -u ext4_client_module_init
