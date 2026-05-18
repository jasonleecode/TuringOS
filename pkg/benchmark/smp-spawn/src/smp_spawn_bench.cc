/*
 * smp-spawn-bench: SMP scheduler / Switch_lock stress test
 *
 * Exercises the Switch_lock::schedule() path fixed by commit e61d5ec:
 * repeated task create/destroy via spawnd combined with cross-CPU IPC noise
 * stresses preemption_point() windows in Context::schedule().
 *
 * When invoked as: smp-spawn-bench --child
 *   Immediately exits — child process spawned by the stress loop.
 *
 * Usage: run rom/smp-spawn-bench [-D <secs>] [-s <spawners>] [-p <prio>]
 *   -D <secs>      duration in seconds        (default: 30)
 *   -s <spawners>  number of spawn threads    (default: 2)
 *   -p <prio>      base thread priority       (default: 10)
 */

#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory.h>
#include <l4/sys/ipc_gate>
#include <l4/sys/ipc_gate.h>
#include <l4/sys/ipc.h>
#include <l4/sys/scheduler>
#include <l4/util/util.h>
#include <pthread.h>
#include <pthread-l4.h>
#include <spawn_ipc.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static void set_affinity(l4_cap_idx_t thread_cap, unsigned cpu, unsigned prio)
{
    l4_sched_param_t sp = l4_sched_param(prio);
    sp.affinity = l4_sched_cpu_set(cpu, 0, 1);
    L4Re::Env::env()->scheduler()->run_thread(
        L4::Cap<L4::Thread>(thread_cap), sp);
}

// ---- Background IPC echo pair (generates cross-CPU IPIs as noise) ----------

struct Ipc_pair {
    l4_cap_idx_t gate_cap;
    volatile bool stop;
    pthread_t srv_tid;
    pthread_t cli_tid;
};

static void *ipc_server(void *arg)
{
    Ipc_pair *p = (Ipc_pair *)arg;
    l4_umword_t label;
    l4_utcb_t *u = l4_utcb();
    /* 100 ms receive timeout lets us notice stop without a drain message. */
    l4_timeout_t to = l4_timeout(L4_IPC_TIMEOUT_0, l4_timeout_from_us(100000));
    l4_ipc_wait(u, &label, L4_IPC_NEVER);
    bool has_msg = true;
    while (!p->stop) {
        l4_msgtag_t r = l4_ipc_reply_and_wait(u, l4_msgtag(0, 0, 0, 0), &label, to);
        has_msg = !l4_msgtag_has_error(r);
    }
    /* Reply to the last unacknowledged message if there is one. */
    if (has_msg)
        l4_ipc_reply_and_wait(u, l4_msgtag(0, 0, 0, 0), &label,
                               l4_timeout(L4_IPC_TIMEOUT_0, L4_IPC_TIMEOUT_0));
    return nullptr;
}

static void *ipc_client(void *arg)
{
    Ipc_pair *p = (Ipc_pair *)arg;
    l4_utcb_t *u = l4_utcb();
    l4_timeout_t to = l4_timeout(l4_timeout_from_us(100000), l4_timeout_from_us(100000));
    while (!p->stop)
        l4_ipc_call(p->gate_cap, u, l4_msgtag(0, 0, 0, 0), to);
    return nullptr;
}

// ---- Spawn stress threads --------------------------------------------------

struct Spawn_arg {
    L4::Cap<Spawn_svr> spawnd;
    volatile bool stop;
    volatile unsigned long count;
    volatile unsigned long fail;
    pthread_t tid;
};

/* argv packed as NUL-separated strings: "smp-spawn-bench\0--child" */
static const char s_child_path[] = "rom/smp-spawn-bench";
static const char s_child_args[] = "smp-spawn-bench\0--child";

static void *spawn_thread(void *arg)
{
    Spawn_arg *a = (Spawn_arg *)arg;
    while (!a->stop) {
        long ret = a->spawnd->spawn(
            L4::Ipc::Array<char const>(sizeof(s_child_path), s_child_path),
            L4::Ipc::Array<char const>(sizeof(s_child_args), s_child_args),
            SPAWN_WAIT);
        if (ret >= 0)
            __atomic_fetch_add(&a->count, 1, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&a->fail,  1, __ATOMIC_RELAXED);
    }
    return nullptr;
}

// ---- Main ------------------------------------------------------------------

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--child") == 0)
        return 0;

    unsigned duration_s = 30;
    unsigned n_spawners = 2;
    unsigned prio       = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0 && i + 1 < argc)
            duration_s = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            n_spawners = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            prio = (unsigned)atoi(argv[++i]);
    }
    if (n_spawners < 1) n_spawners = 1;
    if (n_spawners > 8) n_spawners = 8;

    /* Detect SMP. */
    l4_umword_t cpu_max = 0;
    l4_sched_cpu_set_t cs = l4_sched_cpu_set(0, 0, 1);
    long r = l4_error(L4Re::Env::env()->scheduler()->info(&cpu_max, &cs));
    bool smp = (!r && cpu_max >= 1);

    printf("[smp-spawn-bench] duration=%us spawners=%u prio=%u CPUs=%lu SMP=%s\n",
           duration_s, n_spawners, prio, cpu_max + 1, smp ? "yes" : "no");

    /* Pin main thread to CPU0 at the base priority. */
    set_affinity(pthread_l4_cap(pthread_self()), 0, prio);

    /* Spawnd capability. */
    auto spawnd_cap = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
    bool have_spawnd = spawnd_cap.is_valid();
    if (!have_spawnd)
        printf("[smp-spawn-bench] WARNING: spawnd cap not found\n");

    /* Background IPC gate for cross-CPU noise. */
    auto gate = L4Re::Util::cap_alloc.alloc<L4::Ipc_gate>();
    Ipc_pair ipc_pair = { L4_INVALID_CAP, false, {}, {} };
    if (gate.is_valid()) {
        r = l4_error(l4_factory_create_gate(L4_BASE_FACTORY_CAP,
                                             gate.cap(), L4_INVALID_CAP, 0));
        if (!r) {
            ipc_pair.gate_cap = gate.cap();
            /* IPC server on CPU1 (lower prio = 8 so it doesn't starve spawners).
             * Detached: task teardown cleans them up when main() returns. */
            {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, 8192);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                pthread_create(&ipc_pair.srv_tid, &attr, ipc_server, &ipc_pair);
                pthread_attr_destroy(&attr);
                set_affinity(pthread_l4_cap(ipc_pair.srv_tid),
                             smp ? 1u : 0u, prio - 2);
            }
            l4_sleep(20); /* wait for server to enter l4_ipc_wait */
            /* IPC client on CPU0 (prio - 2, so spawner prio threads run). */
            {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, 8192);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                pthread_create(&ipc_pair.cli_tid, &attr, ipc_client, &ipc_pair);
                pthread_attr_destroy(&attr);
                set_affinity(pthread_l4_cap(ipc_pair.cli_tid), 0, prio - 2);
            }
        }
    }

    /* Spawner threads — not pinned, let scheduler place them on any CPU.
     * Prio = base so they are scheduled normally. */
    static Spawn_arg spawners[8];
    for (unsigned i = 0; i < n_spawners; i++) {
        spawners[i].spawnd = spawnd_cap;
        spawners[i].stop   = false;
        spawners[i].count  = 0;
        spawners[i].fail   = 0;
        if (have_spawnd) {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 16384);
            pthread_create(&spawners[i].tid, &attr, spawn_thread, &spawners[i]);
            pthread_attr_destroy(&attr);
        }
    }

    printf("[smp-spawn-bench] stress running — kernel panic = fix regressed\n");

    /* Sleep in 5s chunks so progress is visible. */
    unsigned slept = 0;
    while (slept < duration_s) {
        unsigned step = (duration_s - slept > 5) ? 5u : (duration_s - slept);
        l4_sleep(step * 1000);
        slept += step;

        unsigned long sp = 0, sf = 0;
        for (unsigned i = 0; i < n_spawners; i++) {
            sp += spawners[i].count;
            sf += spawners[i].fail;
        }
        printf("[smp-spawn-bench] t=%us: spawns=%lu fail=%lu\n", slept, sp, sf);
    }

    /* Signal IPC pair to wind down (detached — task teardown cleans up). */
    ipc_pair.stop = true;

    /* Stop spawners. */
    for (unsigned i = 0; i < n_spawners; i++)
        spawners[i].stop = true;
    for (unsigned i = 0; i < n_spawners; i++)
        if (have_spawnd)
            pthread_join(spawners[i].tid, nullptr);

    /* Final tally. */
    unsigned long total_spawns = 0, total_fail = 0;
    for (unsigned i = 0; i < n_spawners; i++) {
        total_spawns += spawners[i].count;
        total_fail   += spawners[i].fail;
    }

    printf("\n[smp-spawn-bench] Results (%us, %lu CPUs, %s):\n",
           duration_s, cpu_max + 1, smp ? "cross-CPU IPC" : "same-CPU IPC");
    if (have_spawnd)
        printf("  Spawn cycles    : %lu ok + %lu fail = %.1f /s\n",
               total_spawns, total_fail,
               (double)(total_spawns + total_fail) / duration_s);
    else
        printf("  Spawns          : skipped (no spawnd cap)\n");
    printf("[smp-spawn-bench] PASS — survived %us without kernel panic\n",
           duration_s);
    return 0;
}
