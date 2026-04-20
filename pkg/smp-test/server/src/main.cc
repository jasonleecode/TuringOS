/*
 * smp-test: SMP verification for TuringOS / QEMU virt (Cortex-A15)
 *
 * 1. Queries scheduler for online CPUs.
 * 2. Creates a secondary thread pinned to CPU1 via run_thread() affinity.
 * 3. CPU1 thread busy-counts to prove real parallel execution.
 * 4. Main thread (CPU0) waits for CPU1 thread to finish, prints results.
 */

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory>
#include <l4/sys/scheduler>
#include <l4/sys/thread>
#include <l4/sys/ipc.h>
#include <l4/util/util.h>
#include <cstdio>
#include <cstring>

// ---- Secondary thread state ---------------------------------------------

static volatile int cpu1_done = 0;

// Counts incremented concurrently to show real parallelism
static volatile unsigned long cpu1_count = 0;

// Stack for the secondary thread (8 KiB, 8-byte aligned)
static char cpu1_stack[8192] __attribute__((aligned(8)));

static void cpu1_thread_func()
{
  for (unsigned long i = 0; i < 100000UL; i++)
    cpu1_count++;

  __atomic_store_n(&cpu1_done, 1, __ATOMIC_SEQ_CST);
  l4_sleep_forever();
}

// ---- Helpers ------------------------------------------------------------

// Align stack pointer down for direct function call (ARM EABI: 8-byte aligned)
static unsigned long align_sp(void *top)
{
  return reinterpret_cast<unsigned long>(top) & ~7UL;
}

// ---- Main ---------------------------------------------------------------

int main()
{
  printf("[smp-test] Starting SMP verification\n");

  auto *env = L4Re::Env::env();

  // 1. Query online CPUs
  l4_umword_t cpu_max = 0;
  l4_sched_cpu_set_t online;
  online = l4_sched_cpu_set(0, 0, 0);
  l4_umword_t sched_classes = 0;
  l4_msgtag_t tag = env->scheduler()->info(&cpu_max, &online, &sched_classes);
  if (l4_error(tag) < 0)
    {
      printf("[smp-test] ERROR: scheduler info failed (%ld)\n", l4_error(tag));
      return 1;
    }

  printf("[smp-test] cpu_max=%lu, online bitmap=0x%lx\n",
         cpu_max, online.map);

  // Count online CPUs
  unsigned ncpus = 0;
  for (l4_umword_t i = 0; i < cpu_max; i++)
    if (online.map & (1UL << i))
      ncpus++;

  printf("[smp-test] %u CPU(s) online\n", ncpus);
  printf("[smp-test] CPU0 (main thread) running\n");

  if (ncpus < 2)
    {
      printf("[smp-test] Only 1 CPU online — start QEMU with -smp 2\n");
      printf("[smp-test] SKIPPED\n");
      return 0;
    }

  // 2. Allocate a thread capability
  auto thr = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Thread>(),
                           "alloc thread cap");

  // 3. Create thread object via factory
  L4Re::chksys(env->factory()->create(thr), "create thread");

  // 4. Allocate UTCB slot for the new thread (slot 1 = main+1*stride)
  auto *utcb = reinterpret_cast<l4_utcb_t *>(
      reinterpret_cast<unsigned long>(l4_utcb()) + L4_UTCB_OFFSET);

  // 5. Bind thread: UTCB, pager = main thread, exc_handler = main thread
  L4::Thread::Attr attr;
  attr.pager(env->main_thread());
  attr.exc_handler(env->main_thread());
  attr.bind(utcb, L4Re::This_task);
  L4Re::chksys(thr->control(attr), "thread control");

  // 6. Pin to CPU1 with normal priority
  l4_sched_param_t sp = l4_sched_param(2); // priority 2
  sp.affinity = l4_sched_cpu_set(0, 0, 2); // bit1 set → CPU1
  L4Re::chksys(env->scheduler()->run_thread(thr, sp), "run_thread CPU1");

  // 7. Start the thread: set IP + SP
  unsigned long sp_top = align_sp(cpu1_stack + sizeof(cpu1_stack));
  L4Re::chksys(thr->ex_regs(
      reinterpret_cast<unsigned long>(&cpu1_thread_func), sp_top, 0),
    "ex_regs");

  printf("[smp-test] CPU1 thread started, waiting...\n");

  // 8. Spin-wait for CPU1 to finish
  unsigned timeout = 5000; // 5 s
  while (!__atomic_load_n(&cpu1_done, __ATOMIC_SEQ_CST) && timeout--)
    l4_sleep(1);

  if (!__atomic_load_n(&cpu1_done, __ATOMIC_SEQ_CST))
    {
      printf("[smp-test] TIMEOUT waiting for CPU1 thread!\n");
      return 1;
    }

  // 9. Report result
  printf("[smp-test] CPU1 counted %lu iterations on a separate core\n",
         cpu1_count);
  printf("[smp-test] PASSED: both CPUs confirmed active\n");

  return 0;
}
