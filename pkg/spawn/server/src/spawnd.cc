#include "spawnd.h"

#include <l4/re/error_helper>
#include <l4/sys/ipc.h>
#include <l4/sys/thread.h>
#include <cstdio>
#include <cstring>

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
/* do_spawn — internal: load ELF and start child                       */
/* ------------------------------------------------------------------ */

Child_task *Spawnd::do_spawn(const char *path_str,
                              char *const *argv,
                              char *const *envp)
{
    int argc = 0;
    if (argv) while (argv[argc]) argc++;

    printf("[spawnd] spawning: %s\n", path_str);

    Child_task *slot = _table.alloc(path_str);
    if (!slot) {
        fprintf(stderr, "[spawnd] spawn: task table full\n");
        return nullptr;
    }

    try {
        Spawn_app_model am;
        am.set_args(argc,
                    const_cast<const char *const *>(argv),
                    const_cast<const char *const *>(envp));

        typedef Ldr::Elf_loader<Spawn_app_model, Spawn_dbg> My_loader;
        Spawn_dbg dbg;
        My_loader loader;
        loader.launch(&am, path_str, dbg);

        /* Transfer cap ownership from the app model to the task table slot. */
        l4_cap_idx_t task_idx, thread_idx, rm_idx, gate_idx;
        am.release_caps(&task_idx, &thread_idx, &rm_idx, &gate_idx);

        slot->task        = L4Re::Util::Unique_del_cap<L4::Task>(
                                L4::Cap<L4::Task>(task_idx));
        slot->thread      = L4Re::Util::Unique_del_cap<L4::Thread>(
                                L4::Cap<L4::Thread>(thread_idx));
        slot->rm          = L4Re::Util::Unique_del_cap<L4Re::Rm>(
                                L4::Cap<L4Re::Rm>(rm_idx));
        slot->parent_gate = L4Re::Util::Unique_del_cap<L4::Ipc_gate>(
                                L4::Cap<L4::Ipc_gate>(gate_idx));

        printf("[spawnd] spawned: %s (handle=%u task=%lx)\n",
               path_str, slot->handle, (unsigned long)task_idx);
        return slot;

    } catch (L4::Runtime_error const &e) {
        fprintf(stderr, "[spawnd] spawn failed: %s (%s)\n",
                e.str(), e.extra_str());
        _table.free(slot->handle);
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[spawnd] spawn failed (unknown exception)\n");
        _table.free(slot->handle);
        return nullptr;
    }
}

/* ------------------------------------------------------------------ */
/* do_wait — block on parent_gate until child exits                    */
/* ------------------------------------------------------------------ */

long Spawnd::do_wait(Child_task *t)
{
    l4_msg_regs_t *mr = l4_utcb_mr();
    l4_msgtag_t tag = l4_ipc_receive(t->parent_gate.get().cap(),
                                     l4_utcb(), L4_IPC_NEVER);

    if (l4_msgtag_has_error(tag)) {
        fprintf(stderr, "[spawnd] wait: IPC error %ld\n",
                l4_ipc_error(tag, l4_utcb()));
        return -1;
    }

    /* L4Re libc _exit() sends signal(0, exit_code): mr[0]=0, mr[1]=exit_code */
    int nwords = l4_msgtag_words(tag);
    long code  = (nwords >= 2) ? (long)mr->mr[1]
               : (nwords >= 1) ? (long)mr->mr[0]
               : 0;
    return code;
}

/* ------------------------------------------------------------------ */
/* IPC handlers                                                        */
/* ------------------------------------------------------------------ */

l4_ret_t Spawnd::op_spawn(Spawn_svr::Rights,
                           L4::Ipc::Array_ref<char const> path,
                           L4::Ipc::Array_ref<char const> args,
                           l4_uint32_t                    flags)
{
    /* Extract NUL-terminated path string */
    if (path.length == 0) return -L4_EINVAL;
    char path_buf[512];
    size_t plen = path.length < sizeof(path_buf) - 1
                  ? path.length : sizeof(path_buf) - 1;
    memcpy(path_buf, path.data, plen);
    path_buf[plen] = '\0';

    /* Copy args OUT of the IPC UTCB buffer before any further IPC calls.
     * unpack_args sets argv[i] pointers into this buffer.  Without the copy,
     * loader.launch()'s internal IPC (read_infos, alloc_app_stack, …) would
     * overwrite the UTCB before push_argv_strings() can read the strings. */
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
        l4_uint32_t h = slot->handle;
        _table.free(h);
        return code;
    }

    /* SPAWN_BG: return handle to caller */
    return (l4_ret_t)slot->handle;
}

l4_ret_t Spawnd::op_wait(Spawn_svr::Rights,
                          l4_uint32_t handle,
                          l4_uint32_t flags)
{
    Child_task *t = _table.find(handle);
    if (!t) return -L4_ENOENT;

    if (t->state == Child_task::EXITED) {
        long code = t->exit_code;
        _table.free(handle);
        return (l4_ret_t)code;
    }

    if (flags & WAIT_NOHANG)
        return -L4_EAGAIN;

    long code = do_wait(t);
    _table.free(handle);
    return (l4_ret_t)code;
}

l4_ret_t Spawnd::op_kill(Spawn_svr::Rights, l4_uint32_t handle)
{
    Child_task *t = _table.find(handle);
    if (!t) return -L4_ENOENT;

    printf("[spawnd] kill: handle=%u (%s)\n", handle, t->name);
    _table.free(handle);   /* Unique_del_cap RAII deletes kernel objects */
    return L4_EOK;
}

l4_ret_t Spawnd::op_task_count(Spawn_svr::Rights)
{
    return (l4_ret_t)_table.count();
}

l4_ret_t Spawnd::op_task_stat(Spawn_svr::Rights,
                               l4_uint32_t  slot,
                               l4_uint64_t &cpu_us,
                               l4_uint32_t &state,
                               l4_uint32_t &handle)
{
    Child_task *t = _table.at((int)slot);
    if (!t || t->state == Child_task::FREE)
        return -L4_ENOENT;

    l4_kernel_clock_t kc = 0;
    l4_thread_stats_time(t->thread.get().cap(), &kc);
    cpu_us = (l4_uint64_t)kc;
    state  = (l4_uint32_t)t->state;
    handle = t->handle;
    return L4_EOK;
}
