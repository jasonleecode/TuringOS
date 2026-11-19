/*
 * telnetd — a remote shell over telnet, built on netd + spawnd.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Standalone net utility (net-cluster/tools): no lwIP, no sigma0.  It accepts a
 * TCP connection through netd (tcp_listen/accept), presents that connection as
 * an L4::Vcon (a console), and asks spawnd to launch native_shell with that
 * vcon as its console (spawn_con).  The spawned shell does login + every command
 * over the telnet connection, completely unmodified.
 *
 *   bridge:  telnet client  <--TCP-->  netd  <--RPC-->  telnetd (vcon server)
 *                                                         |  spawn_con
 *                                                         v
 *                                                   native_shell (console = vcon)
 *
 * Phase 1: single session.  Run via `run rom/telnetd [port]` (default 23).
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <pthread.h>
#include <unistd.h>

#include <l4/re/env>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>
#include <l4/re/util/vcon_svr>
#include <l4/re/util/icu_svr>
#include <l4/sys/cxx/ipc_epiface>
#include <l4/sys/vcon>
#include <l4/util/util.h>

#include <l4/turingos/net_ipc.h>
#include <l4/turingos/spawn_ipc.h>

namespace {

/* A console (L4::Vcon) whose bytes come from / go to a netd TCP connection.
 * native_shell uses it as stdin/stdout: vcon_write -> netd send; vcon_read
 * pops from an input ring that the pump thread fills from netd recv. */
class Telnet_vcon
: public L4::Epiface_t<Telnet_vcon, L4::Vcon>,
  public L4Re::Util::Icu_cap_array_svr<Telnet_vcon>,
  public L4Re::Util::Vcon_svr<Telnet_vcon>
{
public:
  /* L4::Vcon inherits L4::Icu; Icu_cap_array_svr supplies it.  One IRQ line:
   * the "read-ready" interrupt that wakes native_shell's blocked read(). */
  Telnet_vcon(L4::Cap<Net_svr> netd, l4_uint32_t conn)
  : Icu_cap_array_svr(1, &_irq), _netd(netd), _conn(conn)
  { pthread_mutex_init(&_m, nullptr); }

  l4_msgtag_t op_dispatch(l4_utcb_t *utcb, l4_msgtag_t tag, L4::Vcon::Rights rights)
  {
    if (tag.words() && (l4_utcb_mr_u(utcb)->mr[0] & 0xffff) == L4_VCON_READ_OP
        && _hung_up.load())
      return l4_msgtag(-L4_ENODEV, 0, 0, 0);
    return Vcon_svr::op_dispatch(utcb, tag, rights);
  }

  void hang_up()
  {
    _hung_up.store(true);
    _irq.trigger();
  }

  /* native_shell writes (stdout) -> send to the telnet client. */
  void vcon_write(const char *buf, unsigned size) noexcept
  {
    char data[L4_VCON_WRITE_SIZE];
    if (size > sizeof(data)) return;
    memcpy(data, buf, size); // buf points into the UTCB reused by send()
    unsigned pos = 0;
    while (pos < size) {
      l4_uint32_t sent = 0;
      unsigned chunk = size - pos;
      if (chunk > 128) chunk = 128;
      if (_netd->send(_conn, L4::Ipc::Array<char const>(chunk, data + pos), sent)
          != L4_EOK || !sent)
        break;
      pos += sent;
    }
  }

  /* native_shell reads (stdin) -> pop from the input ring. */
  unsigned vcon_read(char *buf, unsigned size) noexcept
  {
    pthread_mutex_lock(&_m);
    unsigned n = 0;
    while (_head != _tail && n < size)
      { buf[n++] = _ring[_tail]; _tail = (_tail + 1) % RING; }
    bool more = _head != _tail;
    pthread_mutex_unlock(&_m);
    return more ? n + 1 : n;
  }

  int vcon_set_attr(l4_vcon_attr_t const *) noexcept { return -L4_EOK; }
  int vcon_get_attr(l4_vcon_attr_t *a) noexcept
  { a->i_flags = a->o_flags = a->l_flags = 0; return -L4_EOK; }

  /* Pump-thread side: feed decoded input + wake the reader. */
  void push_input(const char *buf, unsigned n)
  {
    pthread_mutex_lock(&_m);
    for (unsigned i = 0; i < n; ++i)
      {
        unsigned nh = (_head + 1) % RING;
        if (nh != _tail) { _ring[_head] = buf[i]; _head = nh; }
      }
    pthread_mutex_unlock(&_m);
    _irq.trigger();
  }

  L4::Cap<Net_svr> netd() const { return _netd; }
  l4_uint32_t      conn() const { return _conn; }

private:
  std::atomic<bool> _hung_up{false};
  static constexpr unsigned RING = 4096;
  Icu_cap_array_svr::Irq _irq;
  L4::Cap<Net_svr>       _netd;
  l4_uint32_t            _conn;
  char                   _ring[RING];
  volatile unsigned      _head = 0, _tail = 0;
  pthread_mutex_t        _m;
};

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;

struct Session {
  Telnet_vcon &console;
  L4::Cap<Spawn_svr> spawnd;
  l4_uint32_t shell;
};

/* Decode Telnet input. The shell chooses what to display during login. */
static void *pump_thread(void *arg)
{
  auto *session = static_cast<Session *>(arg);
  auto *vc = &session->console;
  auto netd = vc->netd();
  l4_uint32_t conn = vc->conn();

  enum { NORMAL, IAC, IAC_OPT } st = NORMAL;
  bool skip_after_cr = false;

  for (;;)
    {
      char rb[512];
      L4::Ipc::Array<char> a(sizeof(rb), rb);
      long r = netd->recv(conn, sizeof(rb), a);
      if (r == -L4_EAGAIN) continue;
      if (r != L4_EOK || !a.length) break;

      char out[512]; unsigned o = 0;
      for (unsigned i = 0; i < a.length; ++i)
        {
          unsigned char c = (unsigned char)rb[i];
          switch (st)
            {
            case NORMAL:
              if (c == 255) { st = IAC; }                   /* IAC */
              else if (c == '\r') { out[o++] = '\n'; skip_after_cr = true; }
              else if ((c == '\n' || c == 0) && skip_after_cr) { skip_after_cr = false; }
              else { out[o++] = (char)c; skip_after_cr = false; }
              break;
            case IAC:
              if (c >= 251 && c <= 254) st = IAC_OPT;        /* WILL/WONT/DO/DONT */
              else if (c == 255) { out[o++] = (char)255; st = NORMAL; }
              else st = NORMAL;                              /* 2-byte command */
              break;
            case IAC_OPT:
              st = NORMAL;                                   /* skip option byte */
              break;
            }
        }
      if (o) vc->push_input(out, o);
    }
  netd->close(conn);
  vc->hang_up();
  long status = session->spawnd->wait(session->shell, 0);
  _exit(status == 0 ? 0 : 1);
}

} // namespace

int main(int argc, char **argv)
{
  unsigned port = (argc > 1) ? (unsigned)atoi(argv[1]) : 23;

  auto netd   = L4Re::Env::env()->get_cap<Net_svr>("netd");
  auto spawnd = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
  if (!netd.is_valid())   { printf("telnetd: netd unavailable\n");   return 1; }
  if (!spawnd.is_valid()) { printf("telnetd: spawnd unavailable\n"); return 1; }

  l4_uint32_t lh = 0;
  if (netd->tcp_listen(port, lh) != L4_EOK)
    { printf("telnetd: cannot listen on port %u\n", port); return 1; }
  printf("telnetd: listening on port %u\n", port);

  /* Single session: wait for one client. */
  l4_uint32_t conn = 0;
  while (conn == 0)
    {
      if (netd->tcp_accept(lh, conn) != L4_EOK) { netd->close(lh); printf("telnetd: accept failed\n"); return 1; }
      if (conn == 0) l4_usleep(200000);
    }
  netd->close(lh);
  printf("telnetd: client connected\n");

  /* Put the client in character mode without local echo (server echoes). */
  static const char nego[] = { (char)255,(char)251,1, (char)255,(char)251,3 }; /* WILL ECHO, WILL SGA */
  l4_uint32_t sent = 0;
  netd->send(conn, L4::Ipc::Array<char const>(sizeof(nego), nego), sent);
  printf("telnetd: nego sent\n"); fflush(stdout);

  static Telnet_vcon vcon(netd, conn);
  auto vcap = server.registry()->register_obj(&vcon);
  if (!vcap.is_valid()) { netd->close(conn); printf("telnetd: cannot register vcon\n"); return 1; }
  printf("telnetd: vcon registered (0x%lx)\n", vcap.cap()); fflush(stdout);

  /* Launch native_shell with the telnet vcon as its console. */
  const char prog[] = "rom/native_shell";
  const char args[] = "native_shell\0--no-console-echo\0";        /* NUL-separated argv */
  long r = spawnd->spawn_con(
      L4::Ipc::Array<char const>(sizeof(prog) - 1, prog),
      L4::Ipc::Array<char const>(sizeof(args), args),
      0 /* SPAWN_BG */,
      L4::Ipc::Cap<void>(vcap, L4_CAP_FPAGE_RWS));
  if (r < 0)
    { netd->close(conn); printf("telnetd: spawn_con failed (%ld)\n", r); return 1; }
  Session session{vcon, spawnd, (l4_uint32_t)r};
  pthread_t pt;
  if (pthread_create(&pt, nullptr, pump_thread, &session) != 0) {
    netd->close(conn);
    spawnd->kill(session.shell);
    return 1;
  }
  pthread_detach(pt);
  printf("telnetd: shell session started (handle %ld)\n", r);

  /* Serve the vcon (native_shell's console read/write/IRQ-bind IPC). */
  server.loop();
  return 0;
}
