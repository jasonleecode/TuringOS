#pragma once
#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_iface>
#include <l4/sys/cxx/ipc_array>

struct Device_supervisor : L4::Kobject_t<Device_supervisor, L4::Kobject, 0x5905>
{
  L4_INLINE_RPC(long, restart, (L4::Ipc::Array<char const> name));
  using Rpcs = L4::Typeid::Rpcs<restart_t>;
};
