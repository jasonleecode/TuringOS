#pragma once

#include <l4/sys/cxx/ipc_epiface>
#include <spawn_ipc.h>

#include "task_table.h"
#include "app_model.h"

/*
 * Spawnd IPC server object.
 *
 * Phase 1: synchronous foreground exec only.
 *   op_spawn with SPAWN_WAIT launches the program and blocks (in l4_ipc_receive)
 *   until the child exits, then returns the exit code.
 *   op_spawn with SPAWN_BG launches and returns the handle immediately.
 *   op_wait blocks the server loop until the child exits.
 *   op_kill terminates the child immediately.
 *
 * Limitation: because spawnd is single-threaded, a blocking op_wait call
 * (or SPAWN_WAIT) makes the server unavailable to other clients until the
 * child exits.  This is acceptable for Phase 1 (single foreground shell).
 */
class Spawnd : public L4::Epiface_t<Spawnd, Spawn_svr>
{
public:
    Spawnd() = default;

    l4_ret_t op_spawn(Spawn_svr::Rights,
                      L4::Ipc::Array_ref<char const> path,
                      L4::Ipc::Array_ref<char const> args,
                      l4_uint32_t                    flags);

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

private:
    Task_table _table;

    /*
     * Launch a child process. Returns a non-null Child_task* with caps
     * owned by _table on success, nullptr on failure.
     *
     * path_str: NUL-terminated executable path
     * argv:     NULL-terminated array of argument strings
     */
    Child_task *do_spawn(const char *path_str,
                         char *const *argv,
                         char *const *envp);

    /* Block on parent_gate until child sends exit signal; return exit code. */
    static long do_wait(Child_task *t);

    /* Unpack a NUL-separated args buffer into a NULL-terminated argv array.
     * Returns argc. argv must have capacity for at least max_argc+1 entries. */
    static int unpack_args(const char *buf, size_t len,
                           char **argv, int max_argc);
};
