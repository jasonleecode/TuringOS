/*
 * TuringOS — FM/AM radio tuner driver IPC protocol.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Protocol 0x5903: the "radio tuner" device-class interface between the
 * tef6686hn-server driver and its clients (native_shell's `radio` command).
 * Driver-framework Phase 1 — second driver migrated to the server pattern
 * (a stateful, multi-method, I2C device, vs. Phase 0's single-read sensor).
 *
 * The tuner STATE (initialised? tuned frequency?) lives in the server, so it
 * persists across separate `radio` command invocations.
 *
 * (0x5901 = spawnd, 0x5902 = Temp_svr; next free number.)
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_iface>

struct Radio_svr : L4::Kobject_t<Radio_svr, L4::Kobject, 0x5903>
{
  /* Reset + initialise the tuner.  out flags: bit0 = 1 if SIMULATED (no real
   * I2C tuner — e.g. QEMU has no i2c-server), 0 if a real chip. Idempotent. */
  L4_INLINE_RPC(long, init, (l4_uint32_t &flags));

  /* Tune.  band: 0=FM 1=MW 2=LW.  freq_khz in kHz (FM 98000 = 98.0 MHz).
   * out actual_khz: frequency the tuner settled on (after AFC). */
  L4_INLINE_RPC(long, tune,
      (l4_uint32_t band, l4_uint32_t freq_khz, l4_uint32_t &actual_khz));

  /* FM seek.  dir: 0=up 1=down.  out found_khz: station parked on. */
  L4_INLINE_RPC(long, seek, (l4_uint32_t dir, l4_uint32_t &found_khz));

  /* Current status.  out freq_khz; out rssi_x10: signal level x10 dBuV. */
  L4_INLINE_RPC(long, status, (l4_uint32_t &freq_khz, l4_int32_t &rssi_x10));

  /* Mute/unmute audio.  on: 0 = unmute, non-zero = mute. */
  L4_INLINE_RPC(long, mute, (l4_uint32_t on));

  /* Set output level (0..255). */
  L4_INLINE_RPC(long, set_volume, (l4_uint32_t level));

  typedef L4::Typeid::Rpcs<init_t, tune_t, seek_t, status_t,
                           mute_t, set_volume_t> Rpcs;
};
