/*
 * TuringOS spawnd — program loader IPC protocol
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Protocol 0x5901: spawn / wait / kill interface between shell and spawnd.
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_iface>
#include <l4/sys/cxx/ipc_types>
#include <l4/sys/cxx/ipc_array>

/* spawn() flags */
enum {
    SPAWN_BG   = 0,  /* background: return handle immediately */
    SPAWN_WAIT = 1,  /* foreground: spawn + wait, return exit code */
    SPAWN_ROM  = 2,  /* force ROM backend even for absolute paths */
};

/* wait() flags */
enum {
    WAIT_BLOCK  = 0,  /* block until child exits */
    WAIT_NOHANG = 1,  /* non-blocking; returns -L4_EAGAIN if still running */
};

struct Spawn_svr : L4::Kobject_t<Spawn_svr, L4::Kobject, 0x5901>
{
    /*
     * Launch a program.
     *
     * path: NUL-terminated executable path ("rom/hello" or "/ext4/bin/foo")
     * args: argv packed as NUL-separated strings, terminated by an extra NUL
     *       e.g. "hello\0arg1\0arg2\0\0"
     * flags: SPAWN_BG / SPAWN_WAIT / SPAWN_ROM
     *
     * Returns: positive handle (SPAWN_BG) or exit code (SPAWN_WAIT),
     *          or negative L4 error code on failure.
     */
    L4_INLINE_RPC(long, spawn,
        (L4::Ipc::Array<char const> path,
         L4::Ipc::Array<char const> args,
         l4_uint32_t                flags));

    /*
     * Wait for a task to exit.
     *
     * handle: value returned by spawn()
     * flags:  WAIT_BLOCK or WAIT_NOHANG
     *
     * Returns: exit code, or -L4_EAGAIN (NOHANG, still running),
     *          or -L4_ENOENT (unknown handle).
     */
    L4_INLINE_RPC(long, wait,
        (l4_uint32_t handle,
         l4_uint32_t flags));

    /*
     * Kill a running task and free all its resources.
     *
     * Returns: 0 on success, -L4_ENOENT if handle unknown.
     */
    L4_INLINE_RPC(long, kill,
        (l4_uint32_t handle));

    typedef L4::Typeid::Rpcs<spawn_t, wait_t, kill_t> Rpcs;
};
