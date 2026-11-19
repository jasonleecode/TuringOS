// Supervise drivers launched through the restricted driver loader.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <l4/re/env>
#include <l4/re/namespace>
#include <l4/re/util/unique_cap>
#include <l4/sys/ipc.h>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>
#include <l4/sys/kip.h>
#include <l4/util/util.h>
#include <l4/turingos/device_supervisor.h>
#include <spawn_ipc.h>
#include <device_table.h>

namespace {
struct Driver {
  long handle = 0;
  unsigned attempts = 0;
  l4_uint64_t window = 0;
  l4_uint64_t last_response = 0;
  std::atomic<bool> restart{false};
};
Driver drivers[g_device_count];
L4::Cap<Spawn_svr> loader;

class Supervisor : public L4::Epiface_t<Supervisor, Device_supervisor>
{
public:
  long op_restart(Device_supervisor::Rights, L4::Ipc::Array_ref<char const> name)
  {
    for (int i = 0; i < g_device_count; ++i)
      if (strlen(g_device_table[i].name) == name.length
          && !memcmp(g_device_table[i].name, name.data, name.length)) {
        drivers[i].restart.store(true);
        return 0;
      }
    return -L4_ENOENT;
  }
};

void *monitor(void *)
{
  auto registry = L4Re::Env::env()->get_cap<L4Re::Namespace>("dev");
  auto probe = L4Re::Util::make_unique_cap<void>();
  for (;;) {
    auto now = l4_kip_clock(l4re_kip());
    for (int i = 0; i < g_device_count; ++i) {
      auto &d = drivers[i];
      const auto &device = g_device_table[i];
      if (d.restart.exchange(false) && d.handle > 0)
        loader->kill(d.handle);
      if (d.handle > 0) {
        long status = loader->wait(d.handle, WAIT_NOHANG);
        if (status == -L4_EAGAIN) {
          if (!registry.is_valid() || !probe.is_valid()) continue;
          // A reply (including a protocol error) shows that dispatch is alive.
          bool healthy = false;
          if (registry->query(device.name,probe.get(),0) >= 0) {
            auto timeout = l4_timeout_from_us(50000);
            auto tag = l4_ipc_call(probe.get().cap(),l4_utcb(),l4_msgtag(L4_PROTO_META,0,0,0),
                                   l4_timeout(timeout,timeout));
            healthy = !l4_msgtag_has_error(tag);
          }
          if (healthy) d.last_response = now;
          else if (now - d.last_response > 5000000) {
            printf("[devmgr] %s unresponsive, stopping driver\n",device.name);
            loader->kill(d.handle);
          }
          continue;
        }
        if (status < 0 && status != -L4_ENOENT && status != -1) continue;
        printf("[devmgr] %s stopped (%ld)\n", device.name, status);
        d.handle = 0;
      }
      if (now - d.window >= 30000000) {
        d.window = now;
        d.attempts = 0;
      }
      if (d.attempts >= 3) continue;
      ++d.attempts;
      d.last_response = now;
      auto path = L4::Ipc::Array<char const>(strlen(device.driver) + 1, device.driver);
      d.handle = loader->spawn(path, path, SPAWN_BG);
      printf("[devmgr] %s launch %ld (attempt %u/3)\n", device.name, d.handle, d.attempts);
    }
    l4_sleep(250);
  }
}
}

int main()
{
  loader = L4Re::Env::env()->get_cap<Spawn_svr>("drvd");
  if (!loader.is_valid()) return 1;
  static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;
  static Supervisor supervisor;
  if (!server.registry()->register_obj(&supervisor, "svr").is_valid()) return 1;
  pthread_t worker;
  if (pthread_create(&worker, nullptr, monitor, nullptr)) return 1;
  server.loop();
}
