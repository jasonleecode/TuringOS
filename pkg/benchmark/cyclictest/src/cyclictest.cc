/*
 * cyclictest: scheduling latency benchmark for TuringOS / L4Re
 *
 * Measures wakeup latency of l4_ipc_sleep_us() with nanosecond precision
 * (l4_kip_clock_ns).  Reports min/avg/max, stddev, and p50/p90/p99/p99.9.
 *
 * Usage: run cyclictest [-i <us>] [-l <n>] [-t <n>] [-p <prio>] [-b <us>]
 *
 *   -i <us>   sleep interval in microseconds (default: 1000)
 *   -l <n>    loops per thread              (default: 1000)
 *   -t <n>    threads                       (default: 1, max: 8)
 *   -p <prio> thread priority               (default: 10)
 *   -b <us>   log overruns above threshold  (default: 0 = disabled)
 */

#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/sys/scheduler>
#include <l4/sys/ipc.h>
#include <l4/sys/kip.h>
#include <l4/re/env.h>
#include <l4/util/util.h>
#include <pthread.h>
#include <pthread-l4.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ---- Constants ----------------------------------------------------------

static constexpr unsigned MAX_THREADS  = 8;
// 1 us per bucket; bucket[b] covers [b*1000, (b+1)*1000) ns.
// Bucket HIST_BUCKETS-1 is the overflow bucket (>= (HIST_BUCKETS-1) us).
static constexpr unsigned HIST_BUCKETS = 1000;

// ---- Configuration ------------------------------------------------------

struct Config
{
  unsigned long interval_us = 1000;
  unsigned long loops       = 1000;
  unsigned      nthreads    = 1;
  unsigned      priority    = 10;
  unsigned long breakmax_us = 0;
};

static Config cfg;

// ---- Per-thread results -------------------------------------------------

struct Thread_result
{
  long long          min_ns    = 0x7fffffffffffffffLL;
  long long          max_ns    = 0;
  long long          sum_ns    = 0;
  unsigned long long sum_sq_ns = 0; // sum of lat_ns^2; unsigned avoids overflow risk
  unsigned long      loops_done = 0;
  unsigned long      overruns   = 0;
  unsigned long      hist[HIST_BUCKETS] = {};

  void record(long long lat_ns)
  {
    if (lat_ns < min_ns) min_ns = lat_ns;
    if (lat_ns > max_ns) max_ns = lat_ns;
    sum_ns    += lat_ns;
    sum_sq_ns += (unsigned long long)lat_ns * (unsigned long long)lat_ns;
    loops_done++;
    unsigned b = (lat_ns < 0) ? 0
               : (lat_ns / 1000 >= (long long)(HIST_BUCKETS - 1))
                 ? HIST_BUCKETS - 1
               : (unsigned)(lat_ns / 1000);
    hist[b]++;
  }
};

static Thread_result results[MAX_THREADS];
static pthread_t     tids[MAX_THREADS];

// ---- KIP clock (ns) -----------------------------------------------------

static inline long long now_ns()
{
  return (long long)l4_kip_clock_ns(l4re_kip());
}

// ---- Integer square root (Newton's method, no libm needed) --------------

static long long isqrt_ull(unsigned long long n)
{
  if (n == 0) return 0;
  unsigned long long x = n, y = (n >> 1) + 1;
  while (y < x) { x = y; y = (x + n / x) >> 1; }
  return (long long)x;
}

// ---- Statistics helpers -------------------------------------------------

static long long compute_stddev_ns(const Thread_result &r)
{
  if (r.loops_done < 2) return 0;
  // var = E[x^2] - (E[x])^2, computed in ns^2
  // Use unsigned long long arithmetic to avoid signed overflow.
  unsigned long long mean_sq = (unsigned long long)(r.sum_ns / (long long)r.loops_done)
                              * (unsigned long long)(r.sum_ns / (long long)r.loops_done);
  unsigned long long esq     = r.sum_sq_ns / r.loops_done;
  if (esq < mean_sq) return 0;
  return isqrt_ull(esq - mean_sq);
}

// Percentile from a histogram.  pct_x10 = target × 10 (e.g. 990 = p99, 999 = p99.9).
// Returns the midpoint of the bucket (in ns).
static long long percentile_ns(const unsigned long *hist, unsigned long total,
                                unsigned pct_x10)
{
  if (total == 0) return 0;
  unsigned long threshold = (unsigned long)((unsigned long long)total * pct_x10 / 1000);
  unsigned long cumul = 0;
  for (unsigned b = 0; b < HIST_BUCKETS; b++)
    {
      cumul += hist[b];
      if (cumul >= threshold)
        return (long long)b * 1000 + 500; // midpoint of [b*1000, (b+1)*1000) ns
    }
  return (long long)(HIST_BUCKETS - 1) * 1000 + 500;
}

// Print nanoseconds as "X.XXX us" — avoids µ (non-ASCII) for serial safety.
static void pr_us(long long ns)
{
  long long us   = ns / 1000;
  long long frac = ns % 1000;
  if (frac < 0) { us--; frac += 1000; }
  printf("%lld.%03lld us", us, frac);
}

// ---- Worker thread ------------------------------------------------------

static void *thread_entry(void *arg)
{
  unsigned id = (unsigned)(uintptr_t)arg;
  Thread_result &res = results[id];
  long long interval_ns = (long long)cfg.interval_us * 1000;
  long long breakmax_ns = (long long)cfg.breakmax_us * 1000;

  for (unsigned long i = 0; i < cfg.loops; i++)
    {
      long long t0 = now_ns();
      l4_ipc_sleep_us(cfg.interval_us);
      long long t1 = now_ns();

      long long lat = t1 - t0 - interval_ns;
      if (lat < 0) lat = 0;

      res.record(lat);

      if (breakmax_ns && lat > breakmax_ns)
        {
          res.overruns++;
          printf("[cyclictest] T%u OVERRUN: lat=", id);
          pr_us(lat);
          printf(" (loop %lu)\n", i);
        }
    }
  return nullptr;
}

// ---- Thread launcher ----------------------------------------------------

static int launch_thread(unsigned id, unsigned priority)
{
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);

  int r = pthread_create(&tids[id], &attr, thread_entry, (void *)(uintptr_t)id);
  pthread_attr_destroy(&attr);
  if (r != 0)
    {
      printf("[cyclictest] ERROR: pthread_create T%u (%d)\n", id, r);
      return -1;
    }

  l4_sched_param_t sp = l4_sched_param(priority);
  L4Re::Env::env()->scheduler()->run_thread(
    L4::Cap<L4::Thread>(pthread_l4_cap(tids[id])), sp);

  return 0;
}

// ---- Result output ------------------------------------------------------

static void print_stats(const Thread_result &r, const unsigned long *hist,
                         unsigned long n)
{
  if (n == 0) return;
  long long avg  = r.sum_ns / (long long)n;
  long long sd   = compute_stddev_ns(r);
  long long p50  = percentile_ns(hist, n, 500);
  long long p90  = percentile_ns(hist, n, 900);
  long long p99  = percentile_ns(hist, n, 990);
  long long p999 = percentile_ns(hist, n, 999);

  printf("    min=");    pr_us(r.min_ns);
  printf("  avg=");      pr_us(avg);
  printf("  max=");      pr_us(r.max_ns);
  printf("  stddev=");   pr_us(sd);
  printf("\n");
  printf("    p50=");    pr_us(p50);
  printf("  p90=");      pr_us(p90);
  printf("  p99=");      pr_us(p99);
  printf("  p99.9=");    pr_us(p999);
  printf("\n");
}

static void print_results()
{
  // Aggregate totals across all threads.
  Thread_result agg;
  agg.min_ns = 0x7fffffffffffffffLL;
  agg.max_ns = 0;
  static unsigned long agg_hist[HIST_BUCKETS]; // static: avoid 4 KB stack frame
  memset(agg_hist, 0, sizeof(agg_hist));
  unsigned long total_loops    = 0;
  unsigned long total_overruns = 0;

  for (unsigned i = 0; i < cfg.nthreads; i++)
    {
      const Thread_result &r = results[i];
      if (r.min_ns < agg.min_ns)  agg.min_ns = r.min_ns;
      if (r.max_ns > agg.max_ns)  agg.max_ns = r.max_ns;
      agg.sum_ns    += r.sum_ns;
      agg.sum_sq_ns += r.sum_sq_ns;
      total_loops   += r.loops_done;
      total_overruns += r.overruns;
      for (unsigned b = 0; b < HIST_BUCKETS; b++)
        agg_hist[b] += r.hist[b];
    }
  agg.loops_done = total_loops;

  printf("\n=== cyclictest results ===\n");
  printf("  interval : %lu us\n", cfg.interval_us);
  printf("  threads  : %u\n",     cfg.nthreads);
  printf("  loops    : %lu / thread\n", cfg.loops);
  printf("\n");

  for (unsigned i = 0; i < cfg.nthreads; i++)
    {
      printf("  T%u:\n", i);
      print_stats(results[i], results[i].hist, results[i].loops_done);
    }

  if (cfg.nthreads > 1)
    {
      printf("  ALL:\n");
      print_stats(agg, agg_hist, total_loops);
    }

  if (total_overruns)
    printf("  OVERRUNS: %lu (threshold %lu us)\n", total_overruns, cfg.breakmax_us);

  printf("\n--- latency histogram (us) ---\n");
  for (unsigned b = 0; b < HIST_BUCKETS; b++)
    {
      if (!agg_hist[b]) continue;
      if (b < HIST_BUCKETS - 1)
        printf("  %4u us: %lu\n", b, agg_hist[b]);
      else
        printf("  >=%u us: %lu\n", b, agg_hist[b]);
    }
  printf("==========================\n");
}

// ---- Argument parsing ---------------------------------------------------

static void parse_args(int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
    {
      if (!strcmp(argv[i], "-i") && i + 1 < argc)
        cfg.interval_us = strtoul(argv[++i], nullptr, 10);
      else if (!strcmp(argv[i], "-l") && i + 1 < argc)
        cfg.loops = strtoul(argv[++i], nullptr, 10);
      else if (!strcmp(argv[i], "-t") && i + 1 < argc)
        cfg.nthreads = (unsigned)strtoul(argv[++i], nullptr, 10);
      else if (!strcmp(argv[i], "-p") && i + 1 < argc)
        cfg.priority = (unsigned)strtoul(argv[++i], nullptr, 10);
      else if (!strcmp(argv[i], "-b") && i + 1 < argc)
        cfg.breakmax_us = strtoul(argv[++i], nullptr, 10);
    }

  if (cfg.nthreads < 1)           cfg.nthreads = 1;
  if (cfg.nthreads > MAX_THREADS) cfg.nthreads = MAX_THREADS;
  if (cfg.loops < 1)              cfg.loops = 1;
  if (cfg.interval_us < 100)      cfg.interval_us = 100;
}

// ---- Main ---------------------------------------------------------------

int main(int argc, char **argv)
{
  parse_args(argc, argv);

  printf("[cyclictest] interval=%lu us, loops=%lu, threads=%u, prio=%u\n",
         cfg.interval_us, cfg.loops, cfg.nthreads, cfg.priority);

  if (cfg.nthreads == 1)
    {
      thread_entry((void *)0);
    }
  else
    {
      for (unsigned i = 0; i < cfg.nthreads; i++)
        if (launch_thread(i, cfg.priority) < 0)
          {
            printf("[cyclictest] FATAL: failed to launch T%u\n", i);
            return 1;
          }

      for (unsigned i = 0; i < cfg.nthreads; i++)
        pthread_join(tids[i], nullptr);
    }

  print_results();
  return 0;
}
