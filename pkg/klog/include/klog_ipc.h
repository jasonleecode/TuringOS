/*
 * TuringOS klog — syslogd IPC protocol
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Protocol 0x5900: one-way log message delivery from any process to syslogd.
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_iface>
#include <l4/sys/cxx/ipc_types>
#include <l4/sys/cxx/ipc_array>

struct Klog_svr : L4::Kobject_t<Klog_svr, L4::Kobject, 0x5900>
{
    L4_INLINE_RPC(long, log, (l4_uint64_t ts_us,
                               l4_uint8_t  level,
                               l4_uint8_t  facility,
                               L4::Ipc::Array<char const> msg));

    typedef L4::Typeid::Rpcs<log_t> Rpcs;
};
