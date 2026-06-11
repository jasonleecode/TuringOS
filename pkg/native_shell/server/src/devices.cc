// Device registration for native_shell.
// Called from main() after Devfs::init().
// Each device is registered only if its hardware capability is available.

#include <l4/devfs/devfs.h>
#include <l4/devfs/device_file.h>
#include <l4/re/env>

#include "dev_rtc.h"

namespace {

/* Minimal device node for a hardware-server cap (fb, input).  These caps used
 * to sit loose at the environment root; they belong under /dev.  The node is a
 * stub (reads return EOF) — the cap is driven over IPC by the relevant
 * subsystem, not by reading the file; the node exists so the device is
 * discoverable and categorised under /dev. */
class Stub_device_file : public Devfs::Device_file
{
public:
  ssize_t readv(const struct iovec *, int) noexcept override { return 0; }
  ~Stub_device_file() noexcept override {}
};

} // namespace

void setup_devices()
{
  auto *env = L4Re::Env::env();

  /* /dev/rtc0 — always try; works via CLOCK_REALTIME even without RTC cap */
  Devfs::register_device("rtc0", cxx::make_ref_obj<Rtc_device_file>());

  /* /dev/fb0 — the framebuffer (fb-drv Goos server); only in --gpu mode. */
  if (env->get_cap<void>("fb").is_valid())
    Devfs::register_device("fb0", cxx::make_ref_obj<Stub_device_file>());

  /* /dev/input0 — the input bus (vbus_input: keyboard + tablet). */
  if (env->get_cap<void>("input").is_valid())
    Devfs::register_device("input0", cxx::make_ref_obj<Stub_device_file>());

  /* /dev/temp0 — the DS18B20 driver now runs as its own task (ds18b20-server);
   * the `temp` command is an IPC client of it.  Re-exposing it as a cap-backed
   * devfs node is a later phase (devfs <-> driver registry). */

  /* /dev/radio0 — the TEF6686HN driver now runs as its own task
   * (tef6686hn-server); the `radio` command is an IPC client of it. */
}
