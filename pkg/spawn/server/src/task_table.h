#pragma once

#include <l4/re/util/unique_cap>
#include <l4/re/rm>
#include <l4/sys/task>
#include <l4/sys/thread>
#include <l4/sys/ipc_gate>

/*
 * Child task tracking table for spawnd.
 *
 * Handles are 1-based positive integers. The table never reuses a handle
 * slot while the child is still running; free() marks the slot available.
 */

struct Child_task {
    l4_uint32_t handle;        /* 1-based ID returned to clients */
    char        name[64];

    /* Kernel objects — freed on kill() or when wait() returns */
    L4Re::Util::Unique_del_cap<L4::Task>     task;
    L4Re::Util::Unique_del_cap<L4::Thread>   thread;
    L4Re::Util::Unique_del_cap<L4Re::Rm>     rm;
    L4Re::Util::Unique_del_cap<L4::Ipc_gate> parent_gate;

    enum State { FREE = 0, LOADING, RUNNING, EXITED } state;
    long exit_code;
    bool being_waited;  /* do_wait() owns gate-receive; reaper skips it */
};

class Task_table {
public:
    static constexpr int MAX = 32;

    Task_table();

    /* Allocate a free slot and assign a new handle. Returns nullptr if full. */
    Child_task *alloc(const char *name);

    /* Look up by handle. Returns nullptr if not found or FREE. */
    Child_task *find(l4_uint32_t handle);

    /* Mark slot as exited with exit_code; does NOT free caps. */
    void set_exited(l4_uint32_t handle, long code);

    /* Release caps and mark slot FREE. */
    void free(l4_uint32_t handle);

    /* Iterate over all non-FREE slots (for 'jobs'). */
    int count() const;
    Child_task *at(int index);   /* index into raw array, may be FREE */
    int capacity() const { return MAX; }

private:
    Child_task   _tasks[MAX];
    l4_uint32_t  _next_handle;
};
