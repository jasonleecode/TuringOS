#include "spawnd.h"

#include <l4/re/error_helper>
#include <l4/sys/ipc.h>
#include <l4/sys/ipc_gate.h>
#include <l4/sys/thread.h>
#include <cstdio>
#include <cstring>

/* ------------------------------------------------------------------ */
/* Constructor / Destructor                                             */
/* ------------------------------------------------------------------ */

Spawnd::Spawnd(L4Re::Util::Object_registry *registry)
: _registry(registry), _reaper_cap(L4_INVALID_CAP), _stop(false)
{
    _exit_notification.service = this;
    pthread_mutex_init(&_mtx, nullptr);
    pthread_create(&_reaper, nullptr, &Spawnd::reaper_main, this);
    _reaper_cap = pthread_l4_cap(_reaper);
}

bool Spawnd::bind_exit_notification()
{
    return _registry->register_irq_obj(&_exit_notification).is_valid();
}

void Spawnd::Exit_notification::handle_irq()
{
    service->finish_waiters();
}

l4_msgtag_t Spawnd::Child_endpoint::dispatch(l4_msgtag_t tag, unsigned rights,
                                             l4_utcb_t *utcb)
{
    service->_caller = handle;
    auto reply = service->dispatch(tag, rights, utcb);
    service->_caller = 0;
    return reply;
}

Spawnd::Child_endpoint &Spawnd::endpoint(Child_task *task)
{
    return _endpoints[task - _table.at(0)];
}

void Spawnd::free_child(Child_task *task)
{
    _registry->unregister_obj(&endpoint(task));
    _table.free(task->handle);
}

Spawnd::~Spawnd()
{
    _stop = true;
    pthread_join(_reaper, nullptr);
    pthread_mutex_destroy(&_mtx);
}

/* ------------------------------------------------------------------ */
/* Reaper thread                                                        */
/*                                                                     */
/* All parent gates are bound to this thread.  The child's exit path   */
/* calls l4_ipc_call(parent_gate) which delivers here.  We read the    */
/* exit code, mark the slot EXITED and notify a pending waiter, then reply  */
/* (send timeout 0 — harmless if the task was already killed) and loop. */
/* On a 500 ms receive timeout we fall through to crash detection.      */
/* ------------------------------------------------------------------ */

void *Spawnd::reaper_main(void *arg)
{
    auto *self  = static_cast<Spawnd *>(arg);
    l4_utcb_t  *u  = l4_utcb();
    l4_umword_t label = 0;
    /* 500 ms receive timeout for periodic crash detection. */
    l4_timeout_t to = l4_timeout(L4_IPC_TIMEOUT_0,
                                  l4_timeout_from_us(500000));
    l4_msgtag_t tag = l4_ipc_wait(u, &label, to);

    while (!self->_stop) {
        if (!l4_msgtag_has_error(tag)) {
            /* Child sent exit IPC.  The kernel ORs permission bits into the
             * label's lower 2 bits; shift right to recover the handle. */
            l4_uint32_t handle = (l4_uint32_t)(label >> 2);
            l4_msg_regs_t *mr = l4_utcb_mr_u(u);
            int  nw   = l4_msgtag_words(tag);
            long code = (nw >= 2) ? (long)mr->mr[1]
                      : (nw >= 1) ? (long)mr->mr[0] : 0;
            pthread_mutex_lock(&self->_mtx);
            Child_task *t = self->_table.find(handle);
            if (t && t->state != Child_task::EXITED) {
                /* Accept exit IPC in LOADING state too: child can exit
                 * before do_spawn transitions the slot to RUNNING. */
                t->state     = Child_task::EXITED;
                t->exit_code = code;
                if (t->waiter) self->_exit_notification.obj_cap()->trigger();
                printf("[spawnd] reaper: [%u] '%s' exited (code=%ld)\n",
                       t->handle, t->name, code);
            }
            pthread_mutex_unlock(&self->_mtx);

            /* Reply so child's l4_ipc_call returns (send timeout 0:
             * skip safely if child task was already freed). */
            tag = l4_ipc_reply_and_wait(u, l4_msgtag(0, 0, 0, 0),
                                         &label, to);
        } else {
            long err = l4_ipc_error(tag, u);
            if (err == L4_IPC_RETIMEOUT) {
                /* Periodic crash detection via thread liveness. */
                pthread_mutex_lock(&self->_mtx);
                self->reap_once();
                pthread_mutex_unlock(&self->_mtx);
            }
            tag = l4_ipc_wait(u, &label, to);
        }
    }
    return nullptr;
}

void Spawnd::reap_once()
{
    /* _mtx must be held by caller.
     * Detect crashed children by checking thread-cap liveness.
     * Exit-IPC detection is handled by the reaper's IPC loop above. */
    for (int i = 0; i < Task_table::MAX; i++) {
        Child_task *t = _table.at(i);
        if (!t || t->state != Child_task::RUNNING)
            continue;

        l4_kernel_clock_t kc = 0;
        l4_msgtag_t r = l4_thread_stats_time(t->thread.get().cap(), &kc);
        if (l4_msgtag_has_error(r)) {
            t->state     = Child_task::EXITED;
            t->exit_code = -1;
            if (t->waiter) _exit_notification.obj_cap()->trigger();
            printf("[spawnd] reaper: [%u] '%s' crashed (thread dead)\n",
                   t->handle, t->name);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

int Spawnd::unpack_args(const char *buf, size_t len,
                         char **out_argv, int max_argc)
{
    int argc = 0;
    const char *p   = buf;
    const char *end = buf + len;

    while (p < end && *p && argc < max_argc) {
        out_argv[argc++] = const_cast<char *>(p);
        p += strlen(p) + 1;
    }
    out_argv[argc] = nullptr;
    return argc;
}

/* ------------------------------------------------------------------ */
/* do_spawn — load ELF and start child                                 */
/* ------------------------------------------------------------------ */

bool Spawnd::load_image(l4_uint32_t h, const char *path_str, char *const *argv,
                        l4_cap_idx_t *task, l4_cap_idx_t *thread,
                        l4_cap_idx_t *rm, l4_cap_idx_t *gate,
                        l4_cap_idx_t console, l4_cap_idx_t service_gate)
{
    int argc = 0;
    if (argv) while (argv[argc]) argc++;

    /* Inject SPAWND_HANDLE=<h> so the child can identify itself when it calls
     * exec().  The env array + buffer only need to outlive loader.launch(),
     * which copies the strings onto the child's startup stack. */
    char handle_env[32];
    snprintf(handle_env, sizeof(handle_env), "SPAWND_HANDLE=%u", h);
    const char *envp[2] = { handle_env, nullptr };

    try {
        Spawn_app_model am;
        am.spawnd_cap = service_gate;
        am.set_args(argc, const_cast<const char *const *>(argv), envp);
        if (l4_is_valid_cap(console))
            am.set_console_cap(console);   /* telnetd vcon etc. */

        /* Bind parent gate to reaper BEFORE launching so the child's exit
         * l4_ipc_call(parent) is delivered to the reaper.  Label bits[1:0]
         * must be zero per L4 IPC gate spec; shift handle left by 2. */
        l4_ipc_gate_bind_thread(am.parent_gate_idx(), _reaper_cap,
                                 (l4_umword_t)h << 2);

        typedef Ldr::Elf_loader<Spawn_app_model, Spawn_dbg> My_loader;
        Spawn_dbg dbg;
        My_loader loader;
        loader.launch(&am, path_str, dbg);

        am.release_caps(task, thread, rm, gate);
        return true;
    } catch (L4::Runtime_error const &e) {
        fprintf(stderr, "[spawnd] load '%s' failed: %s (%s)\n",
                path_str, e.str(), e.extra_str());
        return false;
    } catch (...) {
        fprintf(stderr, "[spawnd] load '%s' failed (unknown exception)\n",
                path_str);
        return false;
    }
}

Child_task *Spawnd::do_spawn(const char *path_str,
                              char *const *argv,
                              Child_task::Console console)
{
    /* 1. Allocate slot (state = LOADING) under lock. */
    pthread_mutex_lock(&_mtx);
    Child_task *slot = _table.alloc(path_str);
    if (slot) {
        slot->owner = _caller;
        Child_task *parent = _table.find(_caller);
        slot->console = (!console.is_valid() && parent) ? parent->console : console;
        Child_endpoint &ep = endpoint(slot);
        ep.service = this;
        ep.handle = slot->handle;
        if (!_registry->register_obj(&ep).is_valid()) {
            _table.free(slot->handle);
            slot = nullptr;
        }
    }
    l4_uint32_t h = slot ? slot->handle : 0;
    pthread_mutex_unlock(&_mtx);

    if (!slot) {
        fprintf(stderr, "[spawnd] spawn: task table full\n");
        return nullptr;
    }

    printf("[spawnd] spawning: %s\n", path_str);

    /* 2. Load ELF without holding _mtx (many internal IPC calls). */
    l4_cap_idx_t task_idx, thread_idx, rm_idx, gate_idx;
    if (!load_image(h, path_str, argv, &task_idx, &thread_idx, &rm_idx, &gate_idx,
                    slot->console.get().cap(), endpoint(slot).obj_cap().cap())) {
        pthread_mutex_lock(&_mtx);
        free_child(slot);
        pthread_mutex_unlock(&_mtx);
        return nullptr;
    }

    /* 3. Populate caps under lock; preserve EXITED if reaper already detected
     *    exit during the LOADING phase. */
    pthread_mutex_lock(&_mtx);
    slot->task        = L4Re::Util::Unique_del_cap<L4::Task>(
                            L4::Cap<L4::Task>(task_idx));
    slot->thread      = L4Re::Util::Unique_del_cap<L4::Thread>(
                            L4::Cap<L4::Thread>(thread_idx));
    slot->rm          = L4Re::Util::Unique_del_cap<L4Re::Rm>(
                            L4::Cap<L4Re::Rm>(rm_idx));
    slot->parent_gate = L4Re::Util::Unique_del_cap<L4::Ipc_gate>(
                            L4::Cap<L4::Ipc_gate>(gate_idx));
    if (slot->state == Child_task::LOADING)
        slot->state = Child_task::RUNNING;
    pthread_mutex_unlock(&_mtx);

    printf("[spawnd] spawned: %s (handle=%u task=%lx)\n",
           path_str, h, (unsigned long)task_idx);
    return slot;
}

/* ------------------------------------------------------------------ */
/* op_exec — replace the program behind the caller's handle (exec)     */
/* ------------------------------------------------------------------ */

l4_ret_t Spawnd::op_exec(Spawn_svr::Rights,
                         l4_uint32_t                    handle,
                         L4::Ipc::Array_ref<char const> path,
                         L4::Ipc::Array_ref<char const> args)
{
    if (!_caller || handle != _caller) return -L4_EACCESS;
    if (path.length == 0) return -L4_EINVAL;

    /* Copy path + args out of the UTCB before any further IPC. */
    char path_buf[512];
    size_t plen = path.length < sizeof(path_buf) - 1
                  ? path.length : sizeof(path_buf) - 1;
    memcpy(path_buf, path.data, plen);
    path_buf[plen] = '\0';

    char args_copy[1024];
    size_t copy_len = args.length < sizeof(args_copy) ? args.length : sizeof(args_copy);
    memcpy(args_copy, args.data, copy_len);
    static constexpr int MAX_ARGV = 64;
    char *argv[MAX_ARGV + 1];
    unpack_args(args_copy, copy_len, argv, MAX_ARGV);

    /* Caller must be a live child of ours. */
    pthread_mutex_lock(&_mtx);
    Child_task *slot = _table.find(handle);
    bool ok = slot && slot->state == Child_task::RUNNING;
    pthread_mutex_unlock(&_mtx);
    if (!ok) return -L4_ENOENT;          /* replies; caller keeps old image */

    printf("[spawnd] exec: handle=%u -> %s\n", handle, path_buf);

    /* Load the new program into a fresh task, gate bound to reaper with the
     * SAME handle so wait/kill/exit-detection keep working. */
    l4_cap_idx_t task_idx, thread_idx, rm_idx, gate_idx;
    if (!load_image(handle, path_buf, argv,
                    &task_idx, &thread_idx, &rm_idx, &gate_idx,
                    slot->console.get().cap(), endpoint(slot).obj_cap().cap()))
        return -L4_EIO;                  /* replies; caller keeps old image */

    /* Atomically swap the slot's caps.  Reassigning slot->task destroys the
     * OLD task (the caller) — its thread is blocked here in the exec IPC and
     * vanishes with the task, so we must not reply (ENOREPLY). */
    pthread_mutex_lock(&_mtx);
    // Only this dispatcher releases slots. A fast replacement may already
    // have reported its exit to the reaper while load_image was returning.
    slot->task        = L4Re::Util::Unique_del_cap<L4::Task>(
                            L4::Cap<L4::Task>(task_idx));
    slot->thread      = L4Re::Util::Unique_del_cap<L4::Thread>(
                            L4::Cap<L4::Thread>(thread_idx));
    slot->rm          = L4Re::Util::Unique_del_cap<L4Re::Rm>(
                            L4::Cap<L4Re::Rm>(rm_idx));
    slot->parent_gate = L4Re::Util::Unique_del_cap<L4::Ipc_gate>(
                            L4::Cap<L4::Ipc_gate>(gate_idx));
    pthread_mutex_unlock(&_mtx);

    printf("[spawnd] exec: handle=%u now runs %s (task=%lx)\n",
           handle, path_buf, (unsigned long)task_idx);
    return -L4_ENOREPLY;                  /* caller's task gone; no reply */
}

long Spawnd::wait_locked(Child_task *task, bool nonblocking)
{
    if (task->waiter) return -L4_EBUSY;
    if (task->state == Child_task::EXITED) {
        long code = task->exit_code;
        free_child(task);
        return code;
    }
    if (nonblocking) return -L4_EAGAIN;

    auto reply = server_iface()->take_reply_cap();
    if (!reply) return reply.error();
    task->waiter = cxx::move(reply).result();
    return -L4_ENOREPLY;
}

void Spawnd::finish_waiters()
{
    pthread_mutex_lock(&_mtx);
    for (int i = 0; i < Task_table::MAX; ++i) {
        Child_task *task = _table.at(i);
        if (task->state == Child_task::EXITED && task->waiter) {
            task->waiter.reply(l4_msgtag(task->exit_code, 0, 0, 0));
            free_child(task);
        }
    }
    pthread_mutex_unlock(&_mtx);
}

/* ------------------------------------------------------------------ */
/* IPC handlers                                                        */
/* ------------------------------------------------------------------ */

l4_ret_t Spawnd::op_spawn(Spawn_svr::Rights,
                           L4::Ipc::Array_ref<char const> path,
                           L4::Ipc::Array_ref<char const> args,
                           l4_uint32_t                    flags)
{
    if (path.length == 0) return -L4_EINVAL;

    char path_buf[512];
    size_t plen = path.length < sizeof(path_buf) - 1
                  ? path.length : sizeof(path_buf) - 1;
    memcpy(path_buf, path.data, plen);
    path_buf[plen] = '\0';

    /* Copy args out of the UTCB before any further IPC. */
    char args_copy[1024];
    size_t copy_len = args.length < sizeof(args_copy) ? args.length : sizeof(args_copy);
    memcpy(args_copy, args.data, copy_len);

    static constexpr int MAX_ARGV = 64;
    char *argv[MAX_ARGV + 1];
    unpack_args(args_copy, copy_len, argv, MAX_ARGV);

    return spawn_result(do_spawn(path_buf, argv), flags);
}

long Spawnd::spawn_result(Child_task *slot, l4_uint32_t flags)
{
    if (!slot) return -L4_ENOMEM;
    if (!(flags & SPAWN_WAIT)) return slot->handle;

    pthread_mutex_lock(&_mtx);
    long code = wait_locked(slot, false);
    // The foreground caller cannot collect a task if no reply slot is available.
    if (code == -L4_ENOMEM && slot->state != Child_task::FREE)
        free_child(slot);
    pthread_mutex_unlock(&_mtx);
    return code;
}

l4_ret_t Spawnd::op_spawn_con(Spawn_svr::Rights,
                              L4::Ipc::Array_ref<char const> path,
                              L4::Ipc::Array_ref<char const> args,
                              l4_uint32_t                    flags,
                              L4::Ipc::Snd_fpage             console)
{
    if (path.length == 0) return -L4_EINVAL;

    char path_buf[512];
    size_t plen = path.length < sizeof(path_buf) - 1
                  ? path.length : sizeof(path_buf) - 1;
    memcpy(path_buf, path.data, plen);
    path_buf[plen] = '\0';

    char args_copy[1024];
    size_t copy_len = args.length < sizeof(args_copy) ? args.length : sizeof(args_copy);
    memcpy(args_copy, args.data, copy_len);
    static constexpr int MAX_ARGV = 64;
    char *argv[MAX_ARGV + 1];
    unpack_args(args_copy, copy_len, argv, MAX_ARGV);

    /* The caller mapped its console (vcon) cap into our receive buffer. */
    l4_cap_idx_t console_cap = L4_INVALID_CAP;
    if (console.cap_received())
        console_cap = server_iface()->get_rcv_cap(0).cap();
    if (!l4_is_valid_cap(console_cap))
        return -L4_EINVAL;

    long r = server_iface()->realloc_rcv_cap(0);
    if (r < 0) return r;
    Child_task::Console received{L4::Cap<void>(console_cap)};
    return spawn_result(do_spawn(path_buf, argv, received), flags);
}

l4_ret_t Spawnd::op_wait(Spawn_svr::Rights,
                          l4_uint32_t handle,
                          l4_uint32_t flags)
{
    pthread_mutex_lock(&_mtx);
    Child_task *t = _table.find(handle);
    if (!t) {
        pthread_mutex_unlock(&_mtx);
        return -L4_ENOENT;
    }

    long code = (!_caller || t->owner == _caller)
                ? wait_locked(t, flags & WAIT_NOHANG) : -L4_EACCESS;
    pthread_mutex_unlock(&_mtx);
    return (l4_ret_t)code;
}

l4_ret_t Spawnd::op_kill(Spawn_svr::Rights, l4_uint32_t handle)
{
    pthread_mutex_lock(&_mtx);
    Child_task *t = _table.find(handle);
    if (!t) {
        pthread_mutex_unlock(&_mtx);
        return -L4_ENOENT;
    }

    if (_caller && t->owner != _caller) {
        pthread_mutex_unlock(&_mtx);
        return -L4_EACCESS;
    }
    if (t->waiter)
        t->waiter.reply(l4_msgtag(-L4_ENOENT, 0, 0, 0));

    printf("[spawnd] kill: handle=%u (%s)\n", handle, t->name);
    free_child(t);
    pthread_mutex_unlock(&_mtx);
    return L4_EOK;
}

l4_ret_t Spawnd::op_task_count(Spawn_svr::Rights)
{
    pthread_mutex_lock(&_mtx);
    int n = _table.count();
    pthread_mutex_unlock(&_mtx);
    return (l4_ret_t)n;
}

l4_ret_t Spawnd::op_task_stat(Spawn_svr::Rights,
                               l4_uint32_t  slot,
                               l4_uint64_t &cpu_us,
                               l4_uint32_t &state,
                               l4_uint32_t &handle)
{
    pthread_mutex_lock(&_mtx);
    Child_task *t = _table.at((int)slot);
    if (!t || t->state == Child_task::FREE || t->state == Child_task::LOADING) {
        pthread_mutex_unlock(&_mtx);
        return -L4_ENOENT;
    }
    /* Copy data needed for the kernel call, then release lock. */
    l4_cap_idx_t thread_cap = t->thread.get().cap();
    /* Map internal state to protocol values: 1=running, 2=exited. */
    state  = (t->state == Child_task::RUNNING) ? 1u : 2u;
    handle = t->handle;
    pthread_mutex_unlock(&_mtx);

    l4_kernel_clock_t kc = 0;
    l4_thread_stats_time(thread_cap, &kc);
    cpu_us = (l4_uint64_t)kc;
    return L4_EOK;
}
