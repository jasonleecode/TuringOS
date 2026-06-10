/*
 * ds18b20-server — DS18B20 temperature sensor as an independent L4Re driver.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Driver-framework Phase 0: instead of native_shell linking the ds18b20 lib and
 * bit-banging GPIO with the shell's full cap set, the driver runs here as its
 * own task holding only { svr (its service gate), vbus (GPIO) }.  Clients call
 * the Temp_svr protocol (temp_proto.h) over IPC.
 *
 * On hardware without a DS18B20/GPIO (e.g. QEMU virt) it falls back to a
 * SIMULATED reading so the IPC path is still demonstrable — clearly flagged,
 * never passed off as a real measurement.
 */

#include <cstdio>

#include <l4/re/env>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/sys/cxx/ipc_epiface>
#include <l4/sys/kip.h>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_gpio>

#include <l4/ds18b20/ds18b20.h>
#include <l4/ds18b20/temp_proto.h>

namespace {

enum { DS18B20_GPIO_PIN = 4 };

class Temp_impl : public L4::Epiface_t<Temp_impl, Temp_svr>
{
public:
  explicit Temp_impl(Ds18b20 *sensor) : _sensor(sensor) {}  // nullptr => sim

  long op_read_temp_c100(Temp_svr::Rights, int &c100, l4_uint32_t &flags)
  {
    if (_sensor)
      {
        int v = 0;
        if (_sensor->read_temp_c100(&v) != L4_EOK)
          return -L4_EIO;
        c100  = v;
        flags = 0;                       // real measurement
        return L4_EOK;
      }

    /* Simulated: no real sensor.  Drift a synthetic value a little so repeated
     * reads change — purely to exercise the IPC round-trip. */
    static int t = 2500;
    t += (int)(l4_kip_clock(l4re_kip()) % 7) - 3;
    if (t < 1500) t = 1500;
    if (t > 3500) t = 3500;
    c100  = t;
    flags = 1;                           // simulated
    return L4_EOK;
  }

private:
  Ds18b20 *_sensor;
};

} // namespace

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;

int main()
{
  printf("[ds18b20-server] starting\n");

  /* Only cap we hold for hardware: the vbus carrying the GPIO controller. */
  Ds18b20 *sensor = nullptr;
  auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (vbus.is_valid())
    {
      L4vbus::Device gpio_dev;
      if (vbus->root().device_by_hid(&gpio_dev, "gpio") >= 0)
        {
          static Ds18b20 s(L4vbus::Gpio_pin(gpio_dev, DS18B20_GPIO_PIN));
          if (s.present())
            {
              sensor = &s;
              printf("[ds18b20-server] DS18B20 found on GPIO pin %d\n",
                     DS18B20_GPIO_PIN);
            }
          else
            printf("[ds18b20-server] no DS18B20 on GPIO pin %d — simulated mode\n",
                   DS18B20_GPIO_PIN);
        }
      else
        printf("[ds18b20-server] no GPIO device on vbus — simulated mode\n");
    }
  else
    printf("[ds18b20-server] no 'vbus' cap — simulated mode\n");

  static Temp_impl impl(sensor);
  if (!server.registry()->register_obj(&impl, "svr").is_valid())
    {
      printf("[ds18b20-server] ERROR: 'svr' capability not found — exiting\n");
      return 1;
    }

  printf("[ds18b20-server] ready (%s)\n", sensor ? "real sensor" : "simulated");
  server.loop();
  return 0;
}
