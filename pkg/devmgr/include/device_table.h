/*
 * TuringOS driver-framework — the device model (single source of truth).
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * One static table describing the devices the system manages.  devmgr (Phase 3)
 * iterates it to launch the matching driver; procfs (Phase 4) iterates it to
 * present /sys/devices.  Each row:
 *   name   — the registry name the driver self-registers under (dev/<name>)
 *   cls    — device class
 *   driver — the driver server ROM path devmgr launches
 *
 * (Phase 4 keeps this compile-time static; true dynamic discovery / hotplug is
 * a later refinement — devmgr would maintain the list at runtime.)
 */
#pragma once

struct Dev_entry
{
  char const *name;
  char const *cls;
  char const *driver;
};

static Dev_entry const g_device_table[] = {
  { "temp0",  "sensor", "rom/ds18b20-server"   },
  { "radio0", "tuner",  "rom/tef6686hn-server" },
};

static int const g_device_count =
  (int)(sizeof(g_device_table) / sizeof(g_device_table[0]));
