#pragma once

#include <l4/sys/cxx/ipc_epiface>
#include <l4/sys/types.h>
#include <l4/turingos/spawn_ipc.h>
#include <l4/re/util/object_registry>
#include <pthread.h>
#include <pthread-l4.h>

#include "task_table.h"
#include "app_model.h"
#include "pipes.h"

class Spawnd : public L4::Epiface_t<Spawnd, Spawn_svr>
{
public:
    explicit Spawnd(L4Re::Util::Object_registry *registry);
    bool bind_exit_notification();
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

    long op_pipe_create(Spawn_svr::Rights, unsigned &r, unsigned &w) { return _pipes.create(_caller,r,w); }
    long op_pipe_close(Spawn_svr::Rights, unsigned id);
    long op_pipe_write(Spawn_svr::Rights, unsigned id, L4::Ipc::Array_ref<char const> bytes)
    { return _pipes.write(_caller,id,bytes.data,bytes.length); }
    long op_pipe_read(Spawn_svr::Rights, unsigned id, unsigned limit, L4::Ipc::Array_ref<char> &bytes);
    long op_spawn_io(Spawn_svr::Rights, L4::Ipc::Array_ref<char const> path,
                     L4::Ipc::Array_ref<char const> args, unsigned flags, unsigned input,
                     unsigned output, L4::Ipc::Array_ref<char const> cwd);
    long op_app_lock(Spawn_svr::Rights, unsigned acquire);
private:
    Pipes _pipes;
    unsigned _app_lock = 0;
    char _read_buf[128];
    struct Child_endpoint : L4::Epiface_t0<Spawn_svr> {
        Spawnd *service;
        l4_uint32_t handle;
        l4_msgtag_t dispatch(l4_msgtag_t, unsigned, l4_utcb_t *) override;
    };
    struct Exit_notification : L4::Irqep_t<Exit_notification> {
        Spawnd *service;
        void handle_irq();
    };
    L4Re::Util::Object_registry *_registry;
    Exit_notification _exit_notification;
    Child_endpoint _endpoints[Task_table::MAX];
    // Set only on the dispatch thread. Zero is the boot-time management gate.
    l4_uint32_t _caller = 0;
    Child_endpoint &endpoint(Child_task *task);
    void free_child(Child_task *task);
    void finish_waiters();
    long spawn_result(Child_task *task, l4_uint32_t flags);
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
                         unsigned flags, Child_task::Console console = {}, unsigned input = ~0u,
                         unsigned output = ~0u, const char *cwd = nullptr);

    /*
     * Load `path_str` as a fresh L4 task and bind its parent gate to the reaper
     * with label (h<<2).  Injects SPAWND_HANDLE=<h> into the child env.  On
     * success fills the four cap indices and returns true; on failure returns
     * false (nothing to clean up — the loader unwinds its own allocations).
     * Shared by do_spawn (new slot) and op_exec (replace existing slot).
     */
    bool load_image(Child_task const *slot, const char *path_str, char *const *argv,
                    l4_cap_idx_t *task, l4_cap_idx_t *thread,
                    l4_cap_idx_t *rm, l4_cap_idx_t *gate,
                    l4_cap_idx_t console, l4_cap_idx_t service_gate, unsigned grants);

    // Caller holds _mtx. The reply cap is completed by finish_waiters().
    long wait_locked(Child_task *task, bool nonblocking);

    /* Unpack NUL-separated arg buffer into a NULL-terminated argv array. */
    static int unpack_args(const char *buf, size_t len,
                           char **argv, int max_argc);

    /* Scan table for exited/crashed background children. Called with _mtx held. */
    void reap_once();

    static void *reaper_main(void *arg);
};
