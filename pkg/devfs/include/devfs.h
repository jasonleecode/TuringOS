// Public API for TuringOS devfs.
//
// Call Devfs::init() once from main() before any open("/dev/...").
// Then call register_device() for each hardware node you want to expose.
#pragma once

#include <l4/devfs/device_file.h>
#include <l4/cxx/ref_ptr>

namespace Devfs {

// Mount devfs at /dev and register built-in nodes (null, zero).
// Must be called from main() after libc/VFS initialisation.
int init();

// Expose a device node at /dev/<name>.
// The Ref_ptr keeps the Device_file alive as long as it is registered.
int register_device(const char *name, cxx::Ref_ptr<Device_file> file);

// Remove a device node (reserved for hot-unplug).
int unregister_device(const char *name);

} // namespace Devfs
