#pragma once

#include <l4/sys/cxx/ipc_epiface>
#include <l4/sys/types.h>
#include <spawn_ipc.h>
#include <pthread.h>
#include <pthread-l4.h>

#include "task_table.h"
#include "app_model.h"

class Spawnd : public L4::Epiface_t<Spawnd, Spawn_svr>
{
public:
    Spawnd();
    ~Spawnd();

    l4_ret_t op_spawn(Spawn_svr::Rights,
                      L4::Ipc::Array_ref<char const> path,
                      L4::Ipc::Array_ref<char const> args,
                      l4_uint32_t                    flags);

    l4_ret_t op_spawn_con(Spawn_svr::Rights,
                          L4::Ipc::Array_ref<char const> path,
                          L4::Ipc::Array_ref<char const> args,
                          l4_uint32_t                    flags,
                          L4::Ipc::Snd_fpage             console);

    l4_ret_t op_wait(Spawn_svr::Rights,
                     l4_uint32_t handle,
                     l4_uint32_t flags);

    l4_ret_t op_kill(Spawn_svr::Rights,
                     l4_uint32_t handle);

    l4_ret_t op_task_count(Spawn_svr::Rights);

    l4_ret_t op_task_stat(Spawn_svr::Rights,
                          l4_uint32_t  slot,
                          l4_uint64_t &cpu_us,
                          l4_uint32_t &state,
                          l4_uint32_t &handle);

    l4_ret_t op_exec(Spawn_svr::Rights,
                     l4_uint32_t                    handle,
                     L4::Ipc::Array_ref<char const> path,
                     L4::Ipc::Array_ref<char const> args);

private:
    Task_table      _table;
    pthread_mutex_t _mtx;
    pthread_t       _reaper;
    l4_cap_idx_t    _reaper_cap;
    volatile bool   _stop;

    /*
     * Launch a child ELF.  Allocates a LOADING slot, loads the ELF without
     * holding _mtx, then transitions the slot to RUNNING under _mtx.
     * Returns the slot on success, nullptr on failure (slot already freed).
     */
    Child_task *do_spawn(const char *path_str,
                         char *const *argv,
                         char *const *envp,
                         l4_cap_idx_t console = L4_INVALID_CAP);

    /*
     * Load `path_str` as a fresh L4 task and bind its parent gate to the reaper
     * with label (h<<2).  Injects SPAWND_HANDLE=<h> into the child env.  On
     * success fills the four cap indices and returns true; on failure returns
     * false (nothing to clean up — the loader unwinds its own allocations).
     * Shared by do_spawn (new slot) and op_exec (replace existing slot).
     */
    bool load_image(l4_uint32_t h, const char *path_str, char *const *argv,
                    l4_cap_idx_t *task, l4_cap_idx_t *thread,
                    l4_cap_idx_t *rm, l4_cap_idx_t *gate,
                    l4_cap_idx_t console = L4_INVALID_CAP);

    /*
     * Wait for a child to exit.  Uses a 200 ms timeout loop so a crash is
     * detected within one period via l4_thread_stats_time().
     * Must NOT be called with _mtx held.
     */
    long do_wait(Child_task *t);

    /* Unpack NUL-separated arg buffer into a NULL-terminated argv array. */
    static int unpack_args(const char *buf, size_t len,
                           char **argv, int max_argc);

    /* Scan table for exited/crashed background children. Called with _mtx held. */
    void reap_once();

    static void *reaper_main(void *arg);
};
