/*
 * TuringOS — temperature-sensor driver IPC protocol.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Protocol 0x5902: the minimal "temperature sensor" device-class interface
 * between a sensor driver server (e.g. ds18b20-server) and its clients
 * (native_shell's `temp` command).  This is Phase 0 of the driver framework:
 * the driver runs as its own server with only the caps it needs, and clients
 * talk to it over IPC instead of linking the driver and banging hardware.
 *
 * (0x5901 = spawnd; pick the next free protocol number.)
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_iface>

struct Temp_svr : L4::Kobject_t<Temp_svr, L4::Kobject, 0x5902>
{
  /*
   * Read the current temperature.
   *   out c100:  temperature in 1/100 °C (e.g. 2537 = 25.37 °C)
   *   out flags: bit0 = 1 if this is a SIMULATED reading (no real sensor —
   *              e.g. QEMU virt has no GPIO/DS18B20), 0 if a real measurement
   * Returns L4_EOK on success, or a negative L4 error code.
   */
  L4_INLINE_RPC(long, read_temp_c100, (int &c100, l4_uint32_t &flags));

  typedef L4::Typeid::Rpcs<read_temp_c100_t> Rpcs;
};
