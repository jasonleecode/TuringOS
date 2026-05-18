#include "spawnd.h"

#include <l4/re/error_helper>
#include <l4/sys/ipc.h>
#include <l4/sys/thread.h>
#include <cstdio>
#include <cstring>

/* ------------------------------------------------------------------ */
/* Constructor / Destructor                                             */
/* ------------------------------------------------------------------ */

Spawnd::Spawnd() : _stop(false)
{
    pthread_mutex_init(&_mtx, nullptr);
    pthread_create(&_reaper, nullptr, &Spawnd::reaper_main, this);
}

Spawnd::~Spawnd()
{
    _stop = true;
    pthread_join(_reaper, nullptr);
    pthread_mutex_destroy(&_mtx);
}

/* ------------------------------------------------------------------ */
/* Reaper thread                                                        */
/* ------------------------------------------------------------------ */

void *Spawnd::reaper_main(void *arg)
{
    auto *self = static_cast<Spawnd *>(arg);
    while (!self->_stop) {
        /* Wake every 500 ms to check background children. */
        l4_ipc_sleep(l4_timeout(L4_IPC_TIMEOUT_0, l4_timeout_from_us(500000)));
        pthread_mutex_lock(&self->_mtx);
        self->reap_once();
        pthread_mutex_unlock(&self->_mtx);
    }
    return nullptr;
}

void Spawnd::reap_once()
{
    /* _mtx must be held by caller. */
    l4_msg_regs_t *mr = l4_utcb_mr();

    for (int i = 0; i < Task_table::MAX; i++) {
        Child_task *t = _table.at(i);
        if (!t || t->state != Child_task::RUNNING || t->being_waited)
            continue;

        /* Non-blocking check: did the child send its exit IPC? */
        l4_msgtag_t tag = l4_ipc_receive(
            t->parent_gate.get().cap(), l4_utcb(),
            l4_timeout(L4_IPC_TIMEOUT_0, L4_IPC_TIMEOUT_0));

        if (!l4_msgtag_has_error(tag)) {
            int  nw   = l4_msgtag_words(tag);
            long code = (nw >= 2) ? (long)mr->mr[1]
                      : (nw >= 1) ? (long)mr->mr[0] : 0;
            t->state     = Child_task::EXITED;
            t->exit_code = code;
            printf("[spawnd] reaper: [%u] '%s' exited (code=%ld)\n",
                   t->handle, t->name, code);
            continue;
        }

        /* Crash detection: if thread_stats_time fails the thread cap is dead. */
        l4_kernel_clock_t kc = 0;
        l4_msgtag_t r = l4_thread_stats_time(t->thread.get().cap(), &kc);
        if (l4_msgtag_has_error(r)) {
            t->state     = Child_task::EXITED;
            t->exit_code = -1;
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

Child_task *Spawnd::do_spawn(const char *path_str,
                              char *const *argv,
                              char *const *envp)
{
    int argc = 0;
    if (argv) while (argv[argc]) argc++;

    /* 1. Allocate slot (state = LOADING) under lock. */
    pthread_mutex_lock(&_mtx);
    Child_task *slot = _table.alloc(path_str);
    l4_uint32_t h    = slot ? slot->handle : 0;
    pthread_mutex_unlock(&_mtx);

    if (!slot) {
        fprintf(stderr, "[spawnd] spawn: task table full\n");
        return nullptr;
    }

    printf("[spawnd] spawning: %s\n", path_str);

    /* 2. Load ELF without holding _mtx (many internal IPC calls). */
    try {
        Spawn_app_model am;
        am.set_args(argc,
                    const_cast<const char *const *>(argv),
                    const_cast<const char *const *>(envp));

        typedef Ldr::Elf_loader<Spawn_app_model, Spawn_dbg> My_loader;
        Spawn_dbg dbg;
        My_loader loader;
        loader.launch(&am, path_str, dbg);

        l4_cap_idx_t task_idx, thread_idx, rm_idx, gate_idx;
        am.release_caps(&task_idx, &thread_idx, &rm_idx, &gate_idx);

        /* 3. Transfer caps and transition slot to RUNNING under lock. */
        pthread_mutex_lock(&_mtx);
        slot->task        = L4Re::Util::Unique_del_cap<L4::Task>(
                                L4::Cap<L4::Task>(task_idx));
        slot->thread      = L4Re::Util::Unique_del_cap<L4::Thread>(
                                L4::Cap<L4::Thread>(thread_idx));
        slot->rm          = L4Re::Util::Unique_del_cap<L4Re::Rm>(
                                L4::Cap<L4Re::Rm>(rm_idx));
        slot->parent_gate = L4Re::Util::Unique_del_cap<L4::Ipc_gate>(
                                L4::Cap<L4::Ipc_gate>(gate_idx));
        slot->state       = Child_task::RUNNING;
        pthread_mutex_unlock(&_mtx);

        printf("[spawnd] spawned: %s (handle=%u task=%lx)\n",
               path_str, slot->handle, (unsigned long)task_idx);
        return slot;

    } catch (L4::Runtime_error const &e) {
        fprintf(stderr, "[spawnd] spawn failed: %s (%s)\n",
                e.str(), e.extra_str());
        pthread_mutex_lock(&_mtx);
        _table.free(h);
        pthread_mutex_unlock(&_mtx);
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[spawnd] spawn failed (unknown exception)\n");
        pthread_mutex_lock(&_mtx);
        _table.free(h);
        pthread_mutex_unlock(&_mtx);
        return nullptr;
    }
}

/* ------------------------------------------------------------------ */
/* do_wait — wait for child exit, detect crashes via timeout           */
/* ------------------------------------------------------------------ */

long Spawnd::do_wait(Child_task *t)
{
    /* Tell reaper to skip gate-receive for this slot. */
    pthread_mutex_lock(&_mtx);
    t->being_waited = true;
    pthread_mutex_unlock(&_mtx);

    long result = -1;
    l4_msg_regs_t *mr = l4_utcb_mr();

    for (;;) {
        /* Check if reaper already detected exit/crash between iterations. */
        pthread_mutex_lock(&_mtx);
        if (t->state == Child_task::EXITED) {
            result = t->exit_code;
            pthread_mutex_unlock(&_mtx);
            break;
        }
        pthread_mutex_unlock(&_mtx);

        /* Wait up to 200 ms for child's exit IPC. */
        l4_msgtag_t tag = l4_ipc_receive(
            t->parent_gate.get().cap(), l4_utcb(),
            l4_timeout(L4_IPC_TIMEOUT_0, l4_timeout_from_us(200000)));

        if (!l4_msgtag_has_error(tag)) {
            int nw = l4_msgtag_words(tag);
            result = (nw >= 2) ? (long)mr->mr[1]
                   : (nw >= 1) ? (long)mr->mr[0] : 0;
            break;
        }

        long err = l4_ipc_error(tag, l4_utcb());
        if (err != L4_IPC_RETIMEOUT) {
            fprintf(stderr, "[spawnd] wait: IPC error %ld\n", err);
            result = -1;
            break;
        }

        /* Timeout: check if the child thread is still alive. */
        l4_kernel_clock_t kc = 0;
        if (l4_msgtag_has_error(l4_thread_stats_time(t->thread.get().cap(), &kc))) {
            fprintf(stderr, "[spawnd] wait: [%u] '%s' crashed (thread dead)\n",
                    t->handle, t->name);
            result = -1;
            break;
        }
        /* Thread alive — keep waiting. */
    }

    pthread_mutex_lock(&_mtx);
    t->being_waited = false;
    t->state        = Child_task::EXITED;
    t->exit_code    = result;
    pthread_mutex_unlock(&_mtx);

    return result;
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

    Child_task *slot = do_spawn(path_buf, argv, nullptr);
    if (!slot)
        return -L4_ENOMEM;

    if (flags & SPAWN_WAIT) {
        long code = do_wait(slot);
        pthread_mutex_lock(&_mtx);
        _table.free(slot->handle);
        pthread_mutex_unlock(&_mtx);
        return (l4_ret_t)code;
    }

    /* SPAWN_BG: reaper will monitor from here. */
    return (l4_ret_t)slot->handle;
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

    if (t->state == Child_task::EXITED) {
        long code = t->exit_code;
        _table.free(handle);
        pthread_mutex_unlock(&_mtx);
        return (l4_ret_t)code;
    }

    if (flags & WAIT_NOHANG) {
        pthread_mutex_unlock(&_mtx);
        return -L4_EAGAIN;
    }
    pthread_mutex_unlock(&_mtx);

    /* Blocking wait — do_wait manages being_waited flag internally. */
    long code = do_wait(t);

    pthread_mutex_lock(&_mtx);
    _table.free(handle);
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

    printf("[spawnd] kill: handle=%u (%s)\n", handle, t->name);
    _table.free(handle);
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
