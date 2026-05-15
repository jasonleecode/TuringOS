/*
 * ipcbench: L4Re IPC performance benchmark
 *
 * Measures IPC round-trip latency, context-switch time, throughput,
 * and concurrent pair throughput (hackbench).
 *
 * Usage: run ipcbench [-m <lat|ctx|thru|hack>] [-i <n>] [-D <s>] [-n <n>] [-p <prio>]
 *
 *   -m lat    IPC round-trip latency — same-core + cross-core auto comparison (default)
 *   -m ctx    Context-switch time   — same as lat, reports round-trip/2
 *   -m thru   Single-pair throughput (Kcalls/s)
 *   -m hack   N concurrent pairs    (total Kcalls/s)
 *
 *   -i <n>    iterations for lat/ctx        (default: 10000)
 *   -D <s>    duration in seconds for thru/hack (default: 3)
 *   -n <n>    concurrent pairs for hack     (default: 4, max: 16)
 *   -p <prio> thread priority               (default: 10)
 */

#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory.h>
#include <l4/sys/ipc_gate>
#include <l4/sys/ipc_gate.h>
#include <l4/sys/ipc.h>
#include <l4/sys/kip.h>
#include <l4/sys/scheduler>
#include <l4/re/env.h>
#include <pthread.h>
#include <pthread-l4.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ---- Constants ----------------------------------------------------------

static constexpr unsigned MAX_PAIRS    = 16;
static constexpr unsigned HIST_BUCKETS = 1000; // 1 µs per bucket, [b*1000,(b+1)*1000) ns

// ---- Configuration ------------------------------------------------------

enum Mode { LAT, CTX, THRU, HACK };

struct Config
{
  Mode          mode        = LAT;
  unsigned long iterations  = 10000;
  unsigned      duration_s  = 3;
  unsigned      npairs      = 4;
  unsigned      priority    = 10;
};

static Config cfg;

// ---- Statistics (shared with cyclictest approach) -----------------------

struct Stats
{
  long long          min_ns    = 0x7fffffffffffffffLL;
  long long          max_ns    = 0;
  long long          sum_ns    = 0;
  unsigned long long sum_sq_ns = 0;
  unsigned long      n         = 0;
  unsigned long      hist[HIST_BUCKETS] = {};

  void record(long long ns)
  {
    if (ns < min_ns) min_ns = ns;
    if (ns > max_ns) max_ns = ns;
    sum_ns    += ns;
    sum_sq_ns += (unsigned long long)ns * (unsigned long long)ns;
    n++;
    unsigned b = (ns <= 0) ? 0
               : (ns / 1000 >= (long long)(HIST_BUCKETS - 1))
                 ? HIST_BUCKETS - 1
               : (unsigned)(ns / 1000);
    hist[b]++;
  }
};

static long long isqrt_ull(unsigned long long v)
{
  if (!v) return 0;
  unsigned long long x = v, y = (v >> 1) + 1;
  while (y < x) { x = y; y = (x + v / x) >> 1; }
  return (long long)x;
}

static long long stddev_ns(const Stats &s)
{
  if (s.n < 2) return 0;
  unsigned long long esq    = s.sum_sq_ns / s.n;
  unsigned long long meansq = (unsigned long long)(s.sum_ns / (long long)s.n)
                            * (unsigned long long)(s.sum_ns / (long long)s.n);
  return (esq >= meansq) ? isqrt_ull(esq - meansq) : 0;
}

// pct_x10: target * 10  (500=p50, 900=p90, 990=p99, 999=p99.9)
static long long percentile_ns(const Stats &s, unsigned pct_x10)
{
  if (!s.n) return 0;
  unsigned long threshold = (unsigned long)((unsigned long long)s.n * pct_x10 / 1000);
  unsigned long cumul = 0;
  for (unsigned b = 0; b < HIST_BUCKETS; b++) {
    cumul += s.hist[b];
    if (cumul >= threshold)
      return (long long)b * 1000 + 500;
  }
  return (long long)(HIST_BUCKETS - 1) * 1000 + 500;
}

static void pr_us(long long ns)
{
  long long us = ns / 1000, fr = ns % 1000;
  if (fr < 0) { us--; fr += 1000; }
  printf("%lld.%03lld us", us, fr);
}

static void print_stats(const Stats &s, bool half = false)
{
  if (!s.n) { printf("    (no data)\n"); return; }
  long long div  = half ? 2 : 1;
  long long avg  = (s.sum_ns / (long long)s.n) / div;
  long long sd   = stddev_ns(s) / div;
  long long p50  = percentile_ns(s, 500)  / div;
  long long p90  = percentile_ns(s, 900)  / div;
  long long p99  = percentile_ns(s, 990)  / div;
  long long p999 = percentile_ns(s, 999)  / div;
  long long mn   = s.min_ns / div;
  long long mx   = s.max_ns / div;

  printf("    min=");   pr_us(mn);
  printf("  avg=");     pr_us(avg);
  printf("  max=");     pr_us(mx);
  printf("  stddev=");  pr_us(sd);
  printf("\n");
  printf("    p50=");   pr_us(p50);
  printf("  p90=");     pr_us(p90);
  printf("  p99=");     pr_us(p99);
  printf("  p99.9=");   pr_us(p999);
  printf("\n");
}

// ---- KIP clock ----------------------------------------------------------

static inline long long now_ns()
{
  return (long long)l4_kip_clock_ns(l4re_kip());
}

// ---- IPC gate helpers ---------------------------------------------------

// Returns L4_INVALID_CAP on failure. Caps are not freed on exit (process cleanup).
static L4::Cap<L4::Ipc_gate> create_gate()
{
  auto gate = L4Re::Util::cap_alloc.alloc<L4::Ipc_gate>();
  if (!gate.is_valid()) {
    printf("[ipcbench] ERROR: cap_alloc failed\n");
    return L4::Cap<L4::Ipc_gate>::Invalid;
  }
  long r = l4_error(l4_factory_create_gate(L4_BASE_FACTORY_CAP,
                                            gate.cap(),
                                            L4_INVALID_CAP, 0));
  if (r) {
    printf("[ipcbench] ERROR: create_gate failed (%ld)\n", r);
    return L4::Cap<L4::Ipc_gate>::Invalid;
  }
  return gate;
}

static void set_affinity(l4_cap_idx_t thread_cap, unsigned cpu, unsigned prio)
{
  l4_sched_param_t sp  = l4_sched_param(prio);
  sp.affinity          = l4_sched_cpu_set(cpu, 0, 1);
  L4Re::Env::env()->scheduler()->run_thread(
    L4::Cap<L4::Thread>(thread_cap), sp);
}

// ---- Server thread arg --------------------------------------------------

struct Server_arg
{
  l4_cap_idx_t          gate_cap;
  volatile bool         stop;       // thru/hack: set by main to end server loop
  volatile unsigned long count;     // thru/hack: messages served
  pthread_t             tid;
};

// ---- Server thread (lat/ctx/thru/hack) ----------------------------------

static void *server_thread(void *arg)
{
  Server_arg *a = (Server_arg *)arg;
  l4_umword_t label;
  l4_msgtag_t tag;
  l4_utcb_t  *utcb = l4_utcb();

  // Wait for first client message (gate blocks client until we reach here)
  tag = l4_ipc_wait(utcb, &label, L4_IPC_NEVER);

  while (!a->stop) {
    a->count++;
    tag = l4_ipc_reply_and_wait(utcb, l4_msgtag(0, 0, 0, 0),
                                 &label, L4_IPC_NEVER);
    (void)tag;
  }
  // Final reply to unblock client's last call
  l4_ipc_reply_and_wait(utcb, l4_msgtag(0, 0, 0, 0), &label, L4_IPC_NEVER);
  return nullptr;
}

// ---- Thread launcher ----------------------------------------------------

// pin_cpu: if true, set CPU affinity; false for hack mode where the OS
// scheduler decides placement (avoiding cross-CPU run_thread races).
static int launch_server(Server_arg *arg, unsigned cpu, unsigned prio, bool pin_cpu = true)
{
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&arg->tid, &attr, server_thread, arg);
  pthread_attr_destroy(&attr);
  if (r) { printf("[ipcbench] ERROR: pthread_create (%d)\n", r); return -1; }

  l4_cap_idx_t scap = pthread_l4_cap(arg->tid);
  l4_ipc_gate_bind_thread(arg->gate_cap, scap, 0);
  if (pin_cpu)
    set_affinity(scap, cpu, prio + 1);
  return 0;
}

// ---- SMP detection ------------------------------------------------------

static bool cpu_available(unsigned cpu)
{
  l4_sched_cpu_set_t cs = l4_sched_cpu_set(0, 0, 1);
  l4_umword_t cpu_max   = 0;
  long r = l4_error(L4Re::Env::env()->scheduler()->info(&cpu_max, &cs));
  if (r) return false;
  return cpu < cpu_max && (cs.map >> cpu) & 1;
}

// ---- lat / ctx mode -----------------------------------------------------

static Stats run_lat(unsigned server_cpu, unsigned prio)
{
  Stats s;

  auto gate = create_gate();
  if (!gate.is_valid()) return s;

  Server_arg arg{};
  arg.gate_cap = gate.cap();
  arg.stop     = false;

  if (launch_server(&arg, server_cpu, prio) < 0) return s;

  l4_utcb_t *utcb = l4_utcb();
  l4_msgtag_t tag  = l4_msgtag(0, 0, 0, 0);

  // Warmup: one call to prime caches and ensure server is in ipc_wait
  l4_ipc_call(gate.cap(), utcb, tag, L4_IPC_NEVER);

  for (unsigned long i = 0; i < cfg.iterations; i++) {
    long long t0 = now_ns();
    l4_ipc_call(gate.cap(), utcb, tag, L4_IPC_NEVER);
    long long t1 = now_ns();
    s.record(t1 - t0);
  }

  arg.stop = true;
  l4_ipc_call(gate.cap(), utcb, tag, L4_IPC_NEVER);
  // Server is detached; it stays in ipc_reply_and_wait after this call returns.
  // Process exit cleans it up — acceptable for a benchmark tool.
  return s;
}

static void mode_lat(bool is_ctx)
{
  const char *label = is_ctx ? "ctx (context-switch)" : "lat (IPC round-trip)";
  printf("\n=== ipcbench results ===\n");
  printf("  mode      : %s\n", label);
  printf("  iterations: %lu  priority: %u\n\n", cfg.iterations, cfg.priority);

  // Set client (this thread) to CPU0
  set_affinity(pthread_l4_cap(pthread_self()), 0, cfg.priority);

  // Same-core: server on CPU0
  printf("  same-core  (client=CPU0  server=CPU0):\n");
  Stats sc = run_lat(0, cfg.priority);
  print_stats(sc, is_ctx);

  // Cross-core: server on CPU1 (if available)
  if (cpu_available(1)) {
    printf("  cross-core (client=CPU0  server=CPU1):\n");
    Stats xc = run_lat(1, cfg.priority);
    print_stats(xc, is_ctx);

    if (sc.n && xc.n) {
      long long div    = is_ctx ? 2 : 1;
      long long sc_avg = (sc.sum_ns / (long long)sc.n) / div;
      long long xc_avg = (xc.sum_ns / (long long)xc.n) / div;
      long long delta  = xc_avg - sc_avg;
      long long pct    = sc_avg ? (delta * 100 / sc_avg) : 0;
      printf("\n  cross-core overhead: +");
      pr_us(delta);
      printf(" avg (+%lld%%)\n", pct);
    }
  } else {
    printf("  cross-core: CPU1 not available (run with --smp 2)\n");
  }

  printf("========================\n");
}

// ---- thru mode ----------------------------------------------------------

static void mode_thru()
{
  printf("\n=== ipcbench results ===\n");
  printf("  mode     : thru (single-pair throughput)\n");
  printf("  duration : %u s\n\n", cfg.duration_s);

  auto gate = create_gate();
  if (!gate.is_valid()) return;

  Server_arg arg{};
  arg.gate_cap = gate.cap();
  arg.stop     = false;
  arg.count    = 0;

  if (launch_server(&arg, 0, cfg.priority) < 0) return;

  l4_utcb_t  *utcb = l4_utcb();
  l4_msgtag_t tag   = l4_msgtag(0, 0, 0, 0);

  // Warmup
  l4_ipc_call(gate.cap(), utcb, tag, L4_IPC_NEVER);

  long long t_end = now_ns() + (long long)cfg.duration_s * 1000000000LL;
  unsigned long calls = 0;
  long long t0 = now_ns();

  while (now_ns() < t_end) {
    l4_ipc_call(gate.cap(), utcb, tag, L4_IPC_NEVER);
    calls++;
  }

  long long elapsed_ns = now_ns() - t0;

  arg.stop = true;
  l4_ipc_call(gate.cap(), utcb, tag, L4_IPC_NEVER);

  long long elapsed_us = elapsed_ns / 1000;
  long long calls_s    = (calls * 1000000LL) / (elapsed_us ? elapsed_us : 1);
  printf("  calls    : %lu\n", calls);
  printf("  elapsed  : %lld.%03lld ms\n", elapsed_us / 1000, elapsed_us % 1000);
  printf("  throughput: %lld calls/s  (%lld Kcalls/s)\n", calls_s, calls_s / 1000);
  printf("  avg latency: ");
  pr_us(calls ? elapsed_ns / (long long)calls : 0);
  printf(" per round-trip\n");
  printf("========================\n");
}

// ---- hack mode ----------------------------------------------------------

struct Hack_pair
{
  Server_arg     srv;
  pthread_t      client_tid;
  volatile bool  stop;
  volatile unsigned long count;
};

// File-scope to avoid C++ static-local init guards (which use pthreads
// mutexes and can deadlock before the IPC subsystem is fully ready).
static Hack_pair g_pairs[MAX_PAIRS];
static L4::Cap<L4::Ipc_gate> g_gates[MAX_PAIRS];

static void *hack_client(void *arg)
{
  Hack_pair *p = (Hack_pair *)arg;
  l4_utcb_t  *utcb = l4_utcb();
  l4_msgtag_t tag   = l4_msgtag(0, 0, 0, 0);

  // Warmup
  l4_ipc_call(p->srv.gate_cap, utcb, tag, L4_IPC_NEVER);

  while (!p->stop) {
    l4_ipc_call(p->srv.gate_cap, utcb, tag, L4_IPC_NEVER);
    p->count++;
  }
  return nullptr;
}

static void mode_hack()
{
  unsigned n = (cfg.npairs < 1) ? 1
             : (cfg.npairs > MAX_PAIRS) ? MAX_PAIRS
             : cfg.npairs;

  printf("\n=== ipcbench results ===\n");
  printf("  mode     : hack (%u pairs)\n", n);
  printf("  duration : %u s\n\n", cfg.duration_s);

  Hack_pair          *pairs = g_pairs;
  L4::Cap<L4::Ipc_gate> *gates = g_gates;

  for (unsigned i = 0; i < n; i++) {
    gates[i] = create_gate();
    if (!gates[i].is_valid()) {
      printf("[ipcbench] ERROR: gate alloc failed for pair %u\n", i);
      return;
    }
    pairs[i].srv.gate_cap = gates[i].cap();
    pairs[i].srv.stop     = false;
    pairs[i].srv.count    = 0;
    pairs[i].stop         = false;
    pairs[i].count        = 0;

    if (launch_server(&pairs[i].srv, 0, cfg.priority, false) < 0) return;
  }

  // Start client threads (detached — stopped via pairs[i].stop flag, no join).
  // Client sets its own affinity at startup to avoid cross-CPU run_thread hang.
  for (unsigned i = 0; i < n; i++) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&pairs[i].client_tid, &attr, hack_client, &pairs[i]);
    pthread_attr_destroy(&attr);
  }

  // Let them run for duration_s
  long long t0    = now_ns();
  long long t_end = t0 + (long long)cfg.duration_s * 1000000000LL;
  while (now_ns() < t_end) {
    l4_ipc_sleep_us(100000); // sleep 100ms between checks
  }
  long long elapsed_ns = now_ns() - t0;

  // Signal all client loops to exit on next iteration check.
  // Clients are detached — no join. After stop=true the client finishes its
  // current round-trip (at most one more IPC, ~tens of µs) and returns.
  // We sleep 50ms to let all in-flight calls drain before reading counts.
  for (unsigned i = 0; i < n; i++) pairs[i].stop = true;
  l4_ipc_sleep_us(50000);

  unsigned long total = 0;
  for (unsigned i = 0; i < n; i++) total += pairs[i].count;

  long long elapsed_us = elapsed_ns / 1000;
  long long calls_s    = (total * 1000000LL) / (elapsed_us ? elapsed_us : 1);

  for (unsigned i = 0; i < n; i++)
    printf("  pair %u: %lu calls\n", i, pairs[i].count);

  printf("\n  total calls  : %lu\n", total);
  printf("  elapsed      : %lld.%03lld ms\n", elapsed_us / 1000, elapsed_us % 1000);
  printf("  throughput   : %lld calls/s  (%lld Kcalls/s, all pairs)\n", calls_s, calls_s / 1000);
  printf("  per-pair avg : %lld calls/s  (%lld Kcalls/s)\n", calls_s / (long long)n, calls_s / 1000 / (long long)n);
  printf("========================\n");
}

// ---- Argument parsing ---------------------------------------------------

static void parse_args(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-m") && i + 1 < argc) {
      i++;
      if      (!strcmp(argv[i], "lat"))  cfg.mode = LAT;
      else if (!strcmp(argv[i], "ctx"))  cfg.mode = CTX;
      else if (!strcmp(argv[i], "thru")) cfg.mode = THRU;
      else if (!strcmp(argv[i], "hack")) cfg.mode = HACK;
    } else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
      cfg.iterations = strtoul(argv[++i], nullptr, 10);
    } else if (!strcmp(argv[i], "-D") && i + 1 < argc) {
      cfg.duration_s = (unsigned)strtoul(argv[++i], nullptr, 10);
    } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
      cfg.npairs = (unsigned)strtoul(argv[++i], nullptr, 10);
    } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
      cfg.priority = (unsigned)strtoul(argv[++i], nullptr, 10);
    }
  }
  if (cfg.iterations < 1)  cfg.iterations = 1;
  if (cfg.duration_s  < 1) cfg.duration_s  = 1;
  if (cfg.npairs      < 1) cfg.npairs      = 1;
  if (cfg.npairs > MAX_PAIRS) cfg.npairs = MAX_PAIRS;
}

// ---- Main ---------------------------------------------------------------

int main(int argc, char **argv)
{
  parse_args(argc, argv);

  switch (cfg.mode) {
    case LAT:  mode_lat(false); break;
    case CTX:  mode_lat(true);  break;
    case THRU: mode_thru();     break;
    case HACK: mode_hack();     break;
  }
  return 0;
}
