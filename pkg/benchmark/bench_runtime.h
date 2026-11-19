#pragma once

#include <l4/re/env>
#include <l4/sys/kip.h>
#include <l4/sys/ipc.h>
#include <l4/sys/scheduler>
#include <pthread.h>
#include <pthread-l4.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace Bench {

// A failed worker invalidates the whole run, including callers waiting for it.
[[noreturn]] inline void fail(char const *operation, long error = 0)
{
  fprintf(stderr, "[benchmark] FAIL %s", operation);
  if (error) fprintf(stderr, " (code=%ld)", error);
  fputc('\n', stderr);
  fflush(stderr);
  _exit(1);
}

inline unsigned number(char const *value, unsigned low, unsigned high = UINT_MAX)
{
  char *end;
  errno = 0;
  unsigned long n = strtoul(value, &end, 10);
  if (*value < '0' || *value > '9' || *end || errno || n < low || n > high)
    fail("invalid numeric argument");
  return static_cast<unsigned>(n);
}

inline unsigned long long now_ns()
{ return l4_kip_clock_ns(l4re_kip()); }

inline unsigned long long sleep_until_us(unsigned long long deadline_us)
{
  auto u = l4_utcb();
  unsigned long long wake_ns;
  do {
    auto timeout = l4_timeout(L4_IPC_TIMEOUT_NEVER, l4_timeout_abs_u(deadline_us, 0, u));
    auto tag = l4_ipc_receive(L4_INVALID_CAP, u, timeout);
    auto error = l4_ipc_error(tag, u);
    if (error != L4_IPC_RETIMEOUT) fail("absolute timer wait", error);
    wake_ns = now_ns();
  } while (wake_ns < deadline_us * 1000);
  return wake_ns;
}

inline l4_umword_t online_cpus()
{
  auto cpus = l4_sched_cpu_set(0, 0, ~0UL);
  long rc = l4_error(L4Re::Env::env()->scheduler()->info(nullptr, &cpus));
  if (rc) fail("scheduler info", rc);
  return cpus.map;
}

inline void schedule(pthread_t thread, unsigned priority, int cpu = -1)
{
  auto param = l4_sched_param(priority);
  if (cpu >= 0)
    param.affinity = l4_sched_cpu_set(cpu, 0, 1);
  long rc = l4_error(L4Re::Env::env()->scheduler()->run_thread(
    L4::Cap<L4::Thread>(pthread_l4_cap(thread)), param));
  if (rc) fail("set scheduling parameters", rc);
}

inline pthread_t start(void *(*entry)(void *), void *arg)
{
  pthread_attr_t attr;
  int rc = pthread_attr_init(&attr);
  if (rc) fail("pthread_attr_init", rc);
  rc = pthread_attr_setstacksize(&attr, 16384);
  if (rc) fail("pthread_attr_setstacksize", rc);
  pthread_t tid;
  rc = pthread_create(&tid, &attr, entry, arg);
  pthread_attr_destroy(&attr);
  if (rc) fail("pthread_create", rc);
  return tid;
}

inline void join(pthread_t tid)
{
  int rc = pthread_join(tid, nullptr);
  if (rc) fail("pthread_join", rc);
}
}
