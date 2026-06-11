/*
 * devmgr — TuringOS driver-framework device manager (Phase 3).
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Decides which drivers to run and launches them dynamically — instead of ned
 * hard-wiring each driver's start in the boot config.  Drivers are spawned
 * through a dedicated, *restricted* loader (`drvd`, a second spawnd instance
 * holding only { dev, vbus_gpio }), so each driver inherits just that minimal
 * cap set — not the shell's full one.  Once up, each driver self-registers in
 * the "dev" registry (Phase 2), so the shell finds it by name.
 *
 * Phase 3 uses a static manifest.  Each entry could later carry a
 * compatible/HID that the manager matches against the bus before launching
 * (true probe); QEMU virt has no real sensor hardware, so the simulated
 * drivers are launched unconditionally here.
 */

#include <cstdio>
#include <cstring>

#include <l4/re/env>
#include <spawn_ipc.h>
#include <device_table.h>      /* the shared device model (g_device_table) */

int main()
{
  printf("[devmgr] starting\n");

  auto drvd = L4Re::Env::env()->get_cap<Spawn_svr>("drvd");
  if (!drvd.is_valid())
    {
      printf("[devmgr] ERROR: 'drvd' loader cap not found — exiting\n");
      return 1;
    }

  int launched = 0;
  for (int i = 0; i < g_device_count; ++i)
    {
      char const *path = g_device_table[i].driver;
      /* argv packed as "path\0\0" (path as argv0), per the spawn protocol. */
      char args[160];
      size_t plen = strlen(path);
      if (plen + 2 > sizeof(args))
        continue;
      memcpy(args, path, plen + 1);
      args[plen + 1] = '\0';                       /* double-NUL terminator */

      long h = drvd->spawn(
          L4::Ipc::Array<char const>(plen + 1, path),
          L4::Ipc::Array<char const>(plen + 2, args),
          SPAWN_BG);
      if (h < 0)
        printf("[devmgr] FAILED to launch %s (%ld)\n", path, h);
      else
        {
          printf("[devmgr] launching %s (handle %ld)\n", path, h);
          ++launched;
        }
    }

  printf("[devmgr] %d driver(s) launched\n", launched);
  return 0;
}
