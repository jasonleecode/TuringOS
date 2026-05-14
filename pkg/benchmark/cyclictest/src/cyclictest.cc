/*
 * cyclictest: scheduling latency benchmark for TuringOS / L4Re
 *
 * Measures wakeup latency of l4_ipc_sleep_us() by comparing the requested
 * wakeup time against the actual KIP clock reading after wake.
 *
 * Usage (from ned config):
 *   l:start({...}, "rom/cyclictest", "-i 1000 -l 1000 -t 8")
 *
 *   -i <us>   sleep interval in microseconds (default: 1000)
 *   -l <n>    number of measurement loops per thread (default: 1000)
 *   -t <n>    number of measurement threads (default: 1, max: 8)
 *   -p <prio> thread priority (default: 10)
 *   -b <us>   breakmax: report overruns if latency > threshold (0 = disabled)
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
static constexpr unsigned HIST_BUCKETS = 100;

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
  long long min_us         = 0x7fffffffffffffffLL;
  long long max_us         = 0;
  long long sum_us         = 0;
  unsigned long loops_done = 0;
  unsigned long overruns   = 0;
  unsigned long hist[HIST_BUCKETS] = {};

  void record(long long lat_us)
  {
    if (lat_us < min_us) min_us = lat_us;
    if (lat_us > max_us) max_us = lat_us;
    sum_us += lat_us;
    loops_done++;
    unsigned b = (lat_us < 0) ? 0
               : (lat_us >= (long long)(HIST_BUCKETS - 1)) ? HIST_BUCKETS - 1
               : (unsigned)lat_us;
    hist[b]++;
  }
};

static Thread_result results[MAX_THREADS];
static pthread_t     tids[MAX_THREADS];

// ---- KIP clock ----------------------------------------------------------

static inline long long now_us()
{
  return (long long)l4_kip_clock(l4re_kip());
}

// ---- Worker thread ------------------------------------------------------

static void *thread_entry(void *arg)
{
  unsigned id = (unsigned)(uintptr_t)arg;
  Thread_result &res = results[id];

  for (unsigned long i = 0; i < cfg.loops; i++)
    {
      long long t0 = now_us();
      l4_ipc_sleep_us(cfg.interval_us);
      long long t1 = now_us();

      long long lat = t1 - t0 - (long long)cfg.interval_us;
      if (lat < 0) lat = 0;

      res.record(lat);

      if (cfg.breakmax_us && (unsigned long)lat > cfg.breakmax_us)
        {
          res.overruns++;
          printf("[cyclictest] T%u OVERRUN: lat=%lld us (loop %lu)\n",
                 id, lat, i);
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

static void print_results()
{
  long long  total_min = 0x7fffffffffffffffLL;
  long long  total_max = 0;
  long long  total_sum = 0;
  unsigned long total_loops = 0;
  unsigned long total_overruns = 0;
  unsigned long total_hist[HIST_BUCKETS] = {};

  for (unsigned i = 0; i < cfg.nthreads; i++)
    {
      const Thread_result &r = results[i];
      if (r.min_us < total_min) total_min = r.min_us;
      if (r.max_us > total_max) total_max = r.max_us;
      total_sum    += r.sum_us;
      total_loops  += r.loops_done;
      total_overruns += r.overruns;
      for (unsigned b = 0; b < HIST_BUCKETS; b++)
        total_hist[b] += r.hist[b];
    }

  printf("\n=== cyclictest results ===\n");
  printf("  interval : %lu us\n", cfg.interval_us);
  printf("  threads  : %u\n", cfg.nthreads);
  printf("  loops    : %lu / thread\n", cfg.loops);
  printf("\n");

  for (unsigned i = 0; i < cfg.nthreads; i++)
    {
      const Thread_result &r = results[i];
      long long avg = r.loops_done ? r.sum_us / (long long)r.loops_done : 0;
      printf("  T%u: min=%lld avg=%lld max=%lld us\n",
             i, r.min_us, avg, r.max_us);
    }

  if (cfg.nthreads > 1)
    {
      long long avg = total_loops ? total_sum / (long long)total_loops : 0;
      printf("  ALL: min=%lld avg=%lld max=%lld us\n",
             total_min, avg, total_max);
    }

  if (total_overruns)
    printf("  OVERRUNS: %lu (threshold %lu us)\n", total_overruns, cfg.breakmax_us);

  printf("\n--- latency histogram (us) ---\n");
  for (unsigned b = 0; b < HIST_BUCKETS; b++)
    {
      if (!total_hist[b]) continue;
      if (b < HIST_BUCKETS - 1)
        printf("  %3u us: %lu\n", b, total_hist[b]);
      else
        printf("  >=%u us: %lu\n", b, total_hist[b]);
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
