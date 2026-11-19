// Measure wakeup lateness against a fixed schedule on the KIP monotonic clock.
// cyclictest [-i interval_us] [-l samples] [-t threads] [-p priority] [-b limit_us]
#include "../../bench_runtime.h"
#include "../../latency_stats.h"
#include <l4/sys/ipc.h>
#include <cstring>

namespace {
constexpr unsigned max_threads = 8;
unsigned interval_us = 1000, loops = 1000, nthreads = 1, priority = 10, limit_us = 0;

struct Worker {
  Bench::Latency_stats stats;
  unsigned long long missed = 0, overruns = 0;
  pthread_t tid;
};
Worker workers[max_threads];

void *measure(void *arg)
{
  auto &w = *static_cast<Worker *>(arg);
  Bench::schedule(pthread_self(), priority);
  auto deadline_us = Bench::now_ns() / 1000 + interval_us;
  for (unsigned i = 0; i < loops; ++i) {
    auto deadline_ns = deadline_us * 1000;
    auto wake_ns = Bench::sleep_until_us(deadline_us);
    auto late = wake_ns - deadline_ns;
    w.stats.record(late);
    if (limit_us && late > limit_us * 1000ULL) ++w.overruns;
    auto skipped = late / (interval_us * 1000ULL);
    w.missed += skipped;
    deadline_us += (skipped + 1) * interval_us;
  }
  return nullptr;
}
}

int main(int argc, char **argv)
{
  for (int i = 1; i < argc; ++i) {
    const char *opt = argv[i];
    if (++i == argc) Bench::fail("missing option value");
    if (!strcmp(opt, "-i")) interval_us = Bench::number(argv[i], 100);
    else if (!strcmp(opt, "-l")) loops = Bench::number(argv[i], 1);
    else if (!strcmp(opt, "-t")) nthreads = Bench::number(argv[i], 1, max_threads);
    else if (!strcmp(opt, "-p")) priority = Bench::number(argv[i], 0, 255);
    else if (!strcmp(opt, "-b")) limit_us = Bench::number(argv[i], 0);
    else Bench::fail("unknown cyclictest option");
  }
  printf("[cyclictest] interval=%u us, loops=%u, threads=%u, prio=%u\n",
         interval_us, loops, nthreads, priority);
  if (nthreads == 1) measure(&workers[0]);
  else {
    for (unsigned i = 0; i < nthreads; ++i)
      workers[i].tid = Bench::start(measure, &workers[i]);
    for (unsigned i = 0; i < nthreads; ++i) Bench::join(workers[i].tid);
  }

  static Bench::Latency_stats all;
  unsigned long long missed = 0, overruns = 0;
  puts("\n=== cyclictest results ===");
  for (unsigned i = 0; i < nthreads; ++i) {
    printf("  T%u: samples=%llu missed periods=%llu\n", i, workers[i].stats.count, workers[i].missed);
    workers[i].stats.print();
    all.merge(workers[i].stats);
    overruns += workers[i].overruns;
    missed += workers[i].missed;
  }
  if (nthreads > 1) {
    printf("  ALL: samples=%llu missed periods=%llu\n", all.count, missed);
    all.print();
  }
  if (limit_us) printf("  OVERRUNS: %llu (threshold %u us)\n", overruns, limit_us);
  puts("--- latency histogram (us) ---");
  for (unsigned i = 0; i < all.buckets; ++i)
    if (all.hist[i]) printf("  %s%u us: %llu\n", i + 1 == all.buckets ? ">=" : "", i, all.hist[i]);
  return overruns ? 1 : 0;
}
