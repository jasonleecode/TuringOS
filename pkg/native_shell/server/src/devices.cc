// Device registration for native_shell.
// Called from main() after Devfs::init().
// Each device is registered only if its hardware capability is available.

#include <l4/devfs/devfs.h>
#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory>

#include <stdio.h>

#include "dev_rtc.h"
#include "dev_radio.h"

void setup_devices()
{
  /* /dev/rtc0 — always try; works via CLOCK_REALTIME even without RTC cap */
  Devfs::register_device("rtc0", cxx::make_ref_obj<Rtc_device_file>());

  /* /dev/temp0 — the DS18B20 driver now runs as its own task (ds18b20-server);
   * the `temp` command is an IPC client of it.  Re-exposing it as a cap-backed
   * devfs node is a later phase (devfs <-> driver registry). */

  /* /dev/radio0 — TEF6686HN on I2C (requires 'i2c' factory cap) */
  {
    auto factory = L4Re::Env::env()->get_cap<L4::Factory>("i2c");
    if (factory.is_valid()) {
      auto dev = L4Re::Util::cap_alloc.alloc<I2c_device_ops>();
      if (dev.is_valid()) {
        char addr_str[16];
        snprintf(addr_str, sizeof(addr_str), "addr=%x",
                 (unsigned)Tef6686hn::I2c_addr);
        long r = l4_error(factory->create(dev, 1L)
                          << static_cast<char const *>(addr_str));
        if (r >= 0) {
          Devfs::register_device("radio0",
            cxx::make_ref_obj<Radio_device_file>(dev));
        } else {
          printf("devfs: I2C device create failed (%ld), "
                 "/dev/radio0 not registered\n", r);
          L4Re::Util::cap_alloc.free(dev);
        }
      }
    }
  }
}
