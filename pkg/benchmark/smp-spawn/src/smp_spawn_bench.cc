// Exercise synchronous process creation alongside checked CPU0-to-CPU1 IPC.
// smp-spawn-bench [-D seconds] [-s spawners] [-p base_priority]
#include "../../echo_pair.h"
#include <spawn_ipc.h>
#include <l4/util/util.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace {
constexpr unsigned max_spawners = 8;
std::atomic<bool> running{false};
std::atomic<bool> stopping{false};
Bench::Echo_server echo;
unsigned long long ipc_ok = 0;

void await_start()
{
  while (!running.load(std::memory_order_acquire)) l4_sleep(1);
}

void *ping(void *)
{
  await_start();
  while (!stopping.load(std::memory_order_relaxed)) {
    echo.call();
    ++ipc_ok;
  }
  return nullptr;
}

struct Spawner {
  L4::Cap<Spawn_svr> service;
  pthread_t thread;
  unsigned long long count = 0;
};
Spawner spawners[max_spawners];

void *spawn_children(void *arg)
{
  auto &s = *static_cast<Spawner *>(arg);
  static const char path[] = "rom/smp-spawn-bench";
  static const char args[] = "smp-spawn-bench\0--child";
  await_start();
  while (!stopping.load(std::memory_order_relaxed)) {
    long status = s.service->spawn({sizeof(path), path}, {sizeof(args), args}, SPAWN_WAIT);
    if (status != 0) Bench::fail("child launch or exit", status);
    ++s.count;
  }
  return nullptr;
}
}

int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--child")) return 0;
  unsigned duration = 30, n = 2, priority = 10;
  for (int i = 1; i < argc; ++i) {
    auto opt = argv[i];
    if (++i == argc) Bench::fail("missing option value");
    if (!strcmp(opt, "-D")) duration = Bench::number(argv[i], 1);
    else if (!strcmp(opt, "-s")) n = Bench::number(argv[i], 1, max_spawners);
    else if (!strcmp(opt, "-p")) priority = Bench::number(argv[i], 2, 255);
    else Bench::fail("unknown spawn benchmark option");
  }
  auto online = Bench::online_cpus();
  bool smp = (online & 3) == 3;
  printf("[smp-spawn-bench] duration=%us spawners=%u prio=%u CPUs=%d SMP=%s\n",
         duration, n, priority, __builtin_popcountl(online), smp ? "yes" : "no");
  if (!smp) Bench::fail("CPU0 and CPU1 required");
  auto service = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
  if (!service.is_valid()) Bench::fail("spawnd unavailable");

  Bench::schedule(pthread_self(), priority, 0);
  echo.open(priority - 2, 1);
  auto client = Bench::start(ping, nullptr);
  Bench::schedule(client, priority - 2, 0);
  for (unsigned i = 0; i < n; ++i) {
    auto &s = spawners[i];
    s.service = service;
    s.thread = Bench::start(spawn_children, &s);
    Bench::schedule(s.thread, priority);
  }
  auto start_ns = Bench::now_ns();
  running.store(true, std::memory_order_release);
  for (unsigned remaining = duration; remaining; ) {
    unsigned step = std::min(remaining, 5u);
    l4_sleep(step * 1000);
    remaining -= step;
  }
  stopping.store(true);
  Bench::join(client);
  echo.close();
  unsigned long long total = 0, minimum = ~0ULL;
  for (unsigned i = 0; i < n; ++i) {
    auto &s = spawners[i];
    Bench::join(s.thread);
    printf("  spawner %u: %llu successful child exits\n", i, s.count);
    minimum = std::min(minimum, s.count);
    total += s.count;
  }
  if (!minimum || !ipc_ok) Bench::fail("incomplete workload");
  auto elapsed_ns = Bench::now_ns() - start_ns;
  printf("  Spawn cycles: %llu total = %.1f /s\n", total, total * 1.e9 / elapsed_ns);
  printf("SMP_WORK spawners=%u min_spawns=%llu ipc_ok=%llu errors=0\n", n, minimum, ipc_ok);
  printf("[smp-spawn-bench] PASS — survived %us without kernel panic\n", duration);
  return 0;
}
