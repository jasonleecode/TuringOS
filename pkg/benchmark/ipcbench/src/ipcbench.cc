// ipcbench [-m lat|ctx|thru|hack] [-i iterations] [-D seconds] [-n pairs] [-p priority]
#include "../../echo_pair.h"
#include "../../latency_stats.h"
#include <l4/util/util.h>
#include <atomic>
#include <cstring>

namespace {
constexpr unsigned max_pairs = 16;
unsigned iterations = 10000, duration = 3, num_pairs = 4, priority = 10;

void latency(bool half_rtt)
{
  auto online = Bench::online_cpus();
  if (!(online & 1)) Bench::fail("CPU0 unavailable");
  Bench::schedule(pthread_self(), priority, 0);
  printf("mode: %s, iterations=%u priority=%u\n",
         half_rtt ? "ctx (estimated half IPC round-trip)" : "lat (IPC round-trip)", iterations, priority);
  double local_avg = 0;
  for (int cpu : {0, 1}) {
    if (!(online & (1UL << cpu))) {
      puts("  cross-core: CPU1 unavailable");
      continue;
    }
    Bench::Echo_server server;
    server.open(priority + 1, cpu);
    server.call(); // warm up outside the measured run
    Bench::Latency_stats stats;
    for (unsigned i = 0; i < iterations; ++i) {
      auto start = Bench::now_ns();
      server.call();
      auto finish = Bench::now_ns();
      if (finish < start) Bench::fail("non-monotonic IPC clock");
      stats.record(finish - start);
    }
    server.close();
    printf("  %s (client=CPU0 server=CPU%d): samples=%llu\n",
           cpu ? "cross-core" : "same-core", cpu, stats.count);
    stats.print(half_rtt ? 2 : 1);
    if (cpu == 0) local_avg = stats.mean;
    else {
      double delta = stats.mean - local_avg;
      printf("  cross-core overhead: %+.3f us avg", delta / (half_rtt ? 2000 : 1000));
      if (local_avg > 0) printf(" (%+.1f%%)", delta * 100 / local_avg);
      putchar('\n');
    }
  }
}

struct Pair {
  Bench::Echo_server server;
  pthread_t client;
  unsigned long long calls = 0;
};
Pair pairs[max_pairs];
std::atomic<unsigned> ready{0};
std::atomic<bool> armed{false};
unsigned long long start_us, end_ns;

void *client(void *arg)
{
  auto &pair = *static_cast<Pair *>(arg);
  pair.server.call();
  ready.fetch_add(1);
  while (!armed.load(std::memory_order_acquire)) l4_sleep(1);
  Bench::sleep_until_us(start_us);
  // Only count exchanges completed within the shared measurement interval.
  while (Bench::now_ns() < end_ns) {
    pair.server.call();
    if (Bench::now_ns() <= end_ns) ++pair.calls;
  }
  return nullptr;
}

void throughput(bool concurrent)
{
  unsigned n = concurrent ? num_pairs : 1;
  if (!concurrent && !(Bench::online_cpus() & 1)) Bench::fail("CPU0 unavailable");
  printf("  mode: %s (%u pairs), duration=%u s\n", concurrent ? "hack" : "thru", n, duration);
  for (unsigned i = 0; i < n; ++i) {
    auto &p = pairs[i];
    p.server.open(priority + 1, concurrent ? -1 : 0);
    p.client = Bench::start(client, &p);
    Bench::schedule(p.client, priority, concurrent ? -1 : 0);
  }
  while (ready.load() != n) l4_sleep(1);
  // Arm independent timers before the common start. A synchronous wake of
  // one runnable IPC pair can otherwise delay releasing the remaining pairs.
  start_us = Bench::now_ns() / 1000 + 2000;
  end_ns = start_us * 1000 + duration * 1000000000ULL;
  armed.store(true, std::memory_order_release);
  unsigned long long total = 0;
  for (unsigned i = 0; i < n; ++i) {
    auto &p = pairs[i];
    Bench::join(p.client);
    p.server.close();
    if (!p.calls) Bench::fail("no completed IPC for pair", i);
    total += p.calls;
    printf("  pair %u: %llu calls\n", i, p.calls);
  }
  auto rate = total / duration;
  printf("  total calls: %llu\n  elapsed: %u s\n  throughput: %llu calls/s (%.1f Kcalls/s)\n",
         total, duration, rate, rate / 1000.0);
  if (concurrent) printf("  per-pair avg: %llu calls/s\n", rate / n);
  else printf("  mean completion interval: %.3f us\n", duration * 1.e6 / total);
}
}

int main(int argc, char **argv)
{
  const char *mode = "lat";
  for (int i = 1; i < argc; ++i) {
    auto opt = argv[i];
    if (++i == argc) Bench::fail("missing option value");
    if (!strcmp(opt, "-m")) mode = argv[i];
    else if (!strcmp(opt, "-i")) iterations = Bench::number(argv[i], 1);
    else if (!strcmp(opt, "-D")) duration = Bench::number(argv[i], 1);
    else if (!strcmp(opt, "-n")) num_pairs = Bench::number(argv[i], 1, max_pairs);
    else if (!strcmp(opt, "-p")) priority = Bench::number(argv[i], 0, 254);
    else Bench::fail("unknown ipcbench option");
  }
  puts("=== ipcbench results ===");
  if (!strcmp(mode, "lat") || !strcmp(mode, "ctx")) latency(!strcmp(mode, "ctx"));
  else if (!strcmp(mode, "thru") || !strcmp(mode, "hack")) throughput(!strcmp(mode, "hack"));
  else Bench::fail("unknown ipcbench mode");
  return 0;
}
