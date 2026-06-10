/*
 * tef6686hn-server — TEF6686HN FM/AM radio tuner as an independent L4Re driver.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Driver-framework Phase 1: the radio driver runs as its own task instead of
 * being linked into native_shell.  It holds only { svr, i2c } and the tuner
 * STATE (initialised? current frequency?) lives here, so it persists across
 * separate `radio` command invocations.  Clients call the Radio_svr protocol
 * (radio_proto.h) over IPC.
 *
 * Without a real I2C tuner (e.g. QEMU virt has no i2c-server) it falls back to
 * SIMULATED operation so the IPC path is demonstrable — clearly flagged.
 */

#include <cstdio>

#include <l4/re/env>
#include <l4/re/namespace>
#include <l4/re/util/cap_alloc>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/sys/cxx/ipc_epiface>
#include <l4/sys/factory>

#include <l4/tef6686hn/tef6686hn.h>
#include <l4/tef6686hn/radio_proto.h>

namespace {

/* Mirror native_shell's old radio_open_i2c(): create an I2C device for the
 * tuner via the "i2c" factory cap.  Invalid cap (no i2c-server) => no tuner. */
L4::Cap<I2c_device_ops> open_i2c()
{
  auto factory = L4Re::Env::env()->get_cap<L4::Factory>("i2c");
  if (!factory.is_valid())
    return L4::Cap<I2c_device_ops>::Invalid;

  auto dev = L4Re::Util::cap_alloc.alloc<I2c_device_ops>();
  if (!dev.is_valid())
    return L4::Cap<I2c_device_ops>::Invalid;

  char addr[16];
  snprintf(addr, sizeof(addr), "addr=%x", (unsigned)Tef6686hn::I2c_addr);
  if (l4_error(factory->create(dev, 1L) << static_cast<char const *>(addr)) < 0)
    return L4::Cap<I2c_device_ops>::Invalid;
  return dev;
}

class Radio_impl : public L4::Epiface_t<Radio_impl, Radio_svr>
{
public:
  long op_init(Radio_svr::Rights, l4_uint32_t &flags)
  {
    if (_inited)
      {
        flags = _sim ? 1 : 0;
        return L4_EOK;                     // idempotent
      }

    auto i2c = open_i2c();
    if (i2c.is_valid())
      {
        _radio = new Tef6686hn(i2c);
        if (_radio->init() == L4_EOK)
          {
            _inited = true; _sim = false; flags = 0;
            printf("[tef6686hn-server] TEF6686HN initialised on I2C\n");
            return L4_EOK;
          }
        delete _radio; _radio = nullptr;
      }

    _inited = true; _sim = true; _sim_khz = 98000; flags = 1;
    printf("[tef6686hn-server] no I2C tuner — simulated mode\n");
    return L4_EOK;
  }

  long op_tune(Radio_svr::Rights, l4_uint32_t band, l4_uint32_t freq_khz,
               l4_uint32_t &actual_khz)
  {
    if (!_inited) return -L4_EINVAL;
    if (_sim) { _sim_khz = freq_khz; actual_khz = freq_khz; return L4_EOK; }

    Tef6686hn::Band b = band == 1 ? Tef6686hn::Band::MW
                      : band == 2 ? Tef6686hn::Band::LW
                                  : Tef6686hn::Band::FM;
    long r = _radio->tune(b, freq_khz);
    if (r != L4_EOK) return r;
    l4_uint32_t a = freq_khz;
    _radio->get_frequency(&a);
    actual_khz = a;
    return L4_EOK;
  }

  long op_seek(Radio_svr::Rights, l4_uint32_t dir, l4_uint32_t &found_khz)
  {
    if (!_inited) return -L4_EINVAL;
    if (_sim)
      {
        if (dir == 1)  // down
          _sim_khz = (_sim_khz <= 87700) ? 108000 : _sim_khz - 200;
        else           // up
          _sim_khz = (_sim_khz >= 107800) ? 87500 : _sim_khz + 200;
        found_khz = _sim_khz;
        return L4_EOK;
      }

    long r = _radio->seek(dir == 1 ? Tef6686hn::Seek_dir::Down
                                   : Tef6686hn::Seek_dir::Up);
    if (r != L4_EOK) return r;
    l4_uint32_t f = 0;
    _radio->get_frequency(&f);
    found_khz = f;
    return L4_EOK;
  }

  long op_status(Radio_svr::Rights, l4_uint32_t &freq_khz, l4_int32_t &rssi_x10)
  {
    if (!_inited) return -L4_EINVAL;
    if (_sim) { freq_khz = _sim_khz; rssi_x10 = 600; return L4_EOK; }  // 60.0 dBuV

    l4_uint32_t f = 0;
    _radio->get_frequency(&f);
    freq_khz = f;
    Tef6686hn::Quality q = {};
    _radio->get_quality(&q);
    rssi_x10 = q.rssi;
    return L4_EOK;
  }

  long op_mute(Radio_svr::Rights, l4_uint32_t on)
  {
    if (!_inited) return -L4_EINVAL;
    if (_sim) return L4_EOK;
    return _radio->mute(on != 0);
  }

  long op_set_volume(Radio_svr::Rights, l4_uint32_t level)
  {
    if (!_inited) return -L4_EINVAL;
    if (level > 255) return -L4_EINVAL;
    if (_sim) return L4_EOK;
    return _radio->set_output_level((l4_uint16_t)level);
  }

private:
  Tef6686hn  *_radio   = nullptr;
  bool        _inited  = false;
  bool        _sim     = false;
  l4_uint32_t _sim_khz = 98000;
};

} // namespace

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;

int main()
{
  printf("[tef6686hn-server] starting\n");

  /* Self-allocate a service gate and publish it in the shared "dev" registry
   * under our device name (clients discover us by name). */
  static Radio_impl impl;
  auto gate = server.registry()->register_obj(&impl);
  if (!gate.is_valid())
    {
      printf("[tef6686hn-server] ERROR: cannot allocate service gate — exiting\n");
      return 1;
    }

  auto dev = L4Re::Env::env()->get_cap<L4Re::Namespace>("dev");
  if (dev.is_valid() && dev->register_obj("radio0", gate) >= 0)
    printf("[tef6686hn-server] registered as dev/radio0\n");
  else
    printf("[tef6686hn-server] WARN: 'dev' registry unavailable — not discoverable\n");

  server.loop();
  return 0;
}
