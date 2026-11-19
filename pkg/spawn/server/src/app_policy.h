#pragma once
#include <cstring>
#include <l4/turingos/spawn_ipc.h>

namespace App_policy {
enum : unsigned {
  Net = SPAWN_NET, Data = SPAWN_DATA, Tmp = SPAWN_TMP, Disk = 1u << 8,
    Display = 1u << 9, Devices = 1u << 10, Driver = 1u << 11,
    Auth = 1u << 12, Log = 1u << 13, Clock = 1u << 14, Supervisor = 1u << 15,
    All = Net | Data | Tmp | Disk | Display | Devices | Driver | Auth | Log | Clock | Supervisor
};

inline const char *rom_name(const char *path)
{
  if (!strncmp(path, "rom/", 4)) path += 4;
  return strchr(path, '/') ? "" : path;
}

inline unsigned profile(const char *path, unsigned flags)
{
  if (*path == '/') return flags & (Net | Data | Tmp);
  const char *n = rom_name(path);
  if (!strcmp(n, "native_shell")) return All & ~(Net | Driver | Supervisor);
  if (!strcmp(n, "telnetd")) return Net;
  if (!strcmp(n, "app")) return Disk;
  if (!strcmp(n, "svc")) return Supervisor;
  if (!strcmp(n, "wasm-run")) return Disk | Tmp;
  if (!strcmp(n, "ds18b20-server") || !strcmp(n, "tef6686hn-server")) return Driver;
  if (!strcmp(n, "lvgl-desktop") || !strcmp(n, "lvgl-demo")) return Display | Log | Clock;
  if (!strcmp(n, "cat")) return Disk | Tmp | Devices | Clock | Log;
  const char *const network[] = {"netcat", "ping", "ifconfig", "dhcp", "udpecho", "tcpecho", "mqtt", "nslookup"};
  for (auto name : network) if (!strcmp(n, name)) return Net;
  return 0;
}

inline unsigned delegation(const char *path, unsigned granted, unsigned ceiling)
{
  auto n = rom_name(path);
  if (!strcmp(n, "native_shell") || !strcmp(n, "telnetd")) return ceiling;
  if (!strcmp(n, "app")) return ceiling & (Disk | Data | Tmp | Net);
  return granted;
}

inline bool forward(const char *name, unsigned mask)
{
  if (!strcmp(name, "rom")) return true;
  struct Entry {
    const char *name;
    unsigned bit;
  };
  static const Entry entries[] = {
    {"netd", Net}, {"data", Data}, {"tmp", Tmp}, {"ext4", Disk}, {"fb", Display},
    {"input", Display}, {"dev", Driver | Devices}, {"vbus", Driver}, {"authd", Auth},
    {"syslogd", Log}, {"rtc", Clock}, {"supervisor", Supervisor}
  };
  for (auto &e : entries) if (!strcmp(name, e.name)) return (mask & e.bit) != 0;
  return false;
}
}
