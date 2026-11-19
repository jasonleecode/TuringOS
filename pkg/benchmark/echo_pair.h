#pragma once

#include "bench_runtime.h"
#include <l4/re/util/unique_cap>
#include <l4/sys/factory.h>
#include <l4/sys/ipc_gate>

namespace Bench {

// A synchronous empty message with a distinct reply tag confirms delivery to
// the echo thread. Shutdown is a one-way message, so no final waiter is left.
class Echo_server
{
  enum { Ping = 0, Pong = 1, Stop = 2 };
  L4Re::Util::Unique_del_cap<L4::Ipc_gate> gate;
  pthread_t thread;

  static l4_timeout_t call_timeout()
  {
    auto t = l4_timeout_from_us(1000000);
    return l4_timeout(t, t);
  }

  static void *serve(void *)
  {
    auto u = l4_utcb();
    l4_umword_t label;
    auto tag = l4_ipc_wait(u, &label, L4_IPC_NEVER);
    for (;;) {
      if (l4_msgtag_has_error(tag)) fail("IPC receive/reply", l4_ipc_error(tag, u));
      if (tag.label() == Stop) return nullptr;
      if (tag.label() != Ping) fail("unexpected IPC request", tag.label());
      tag = l4_ipc_reply_and_wait(u, l4_msgtag(Pong, 0, 0, 0), &label, L4_IPC_NEVER);
    }
  }

public:
  void open(unsigned priority, int cpu = -1)
  {
    gate = L4Re::Util::make_unique_del_cap<L4::Ipc_gate>();
    if (!gate.is_valid()) fail("allocate IPC gate");
    thread = start(serve, nullptr);
    schedule(thread, priority, cpu);
    long rc = l4_error(l4_factory_create_gate(L4_BASE_FACTORY_CAP, gate.cap(),
                                              pthread_l4_cap(thread), 0));
    if (rc) fail("create bound IPC gate", rc);
  }

  void call() const
  {
    auto u = l4_utcb();
    auto r = l4_ipc_call(gate.cap(), u, l4_msgtag(Ping, 0, 0, 0), call_timeout());
    if (l4_msgtag_has_error(r)) fail("IPC call", l4_ipc_error(r, u));
    if (r.label() != Pong) fail("unexpected IPC reply", r.label());
  }

  void close()
  {
    auto u = l4_utcb();
    auto r = l4_ipc_send(gate.cap(), u, l4_msgtag(Stop, 0, 0, 0), call_timeout());
    if (l4_msgtag_has_error(r)) fail("IPC shutdown", l4_ipc_error(r, u));
    join(thread);
  }
};
}
