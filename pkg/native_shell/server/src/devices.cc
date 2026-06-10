// Device registration for native_shell.
// Called from main() after Devfs::init().
// Each device is registered only if its hardware capability is available.

#include <l4/devfs/devfs.h>
#include <l4/re/env>

#include "dev_rtc.h"

void setup_devices()
{
  /* /dev/rtc0 — always try; works via CLOCK_REALTIME even without RTC cap */
  Devfs::register_device("rtc0", cxx::make_ref_obj<Rtc_device_file>());

  /* /dev/temp0 — the DS18B20 driver now runs as its own task (ds18b20-server);
   * the `temp` command is an IPC client of it.  Re-exposing it as a cap-backed
   * devfs node is a later phase (devfs <-> driver registry). */

  /* /dev/radio0 — the TEF6686HN driver now runs as its own task
   * (tef6686hn-server); the `radio` command is an IPC client of it. */
}
