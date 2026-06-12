/*
 * mqtt — a minimal MQTT 3.1.1 client over netd.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Standalone net utility (net-cluster/tools): its own task, no lwIP, no sigma0.
 * It speaks MQTT directly (CONNECT/PUBLISH/SUBSCRIBE framing) over netd's TCP
 * transport (Net_svr::tcp_connect/send/recv/close) — the protocol logic lives
 * here, netd just moves bytes.  Replaces the old in-shell `mqtt` builtin (which
 * was glued to lwIP's mqtt app).
 *
 *   run rom/mqtt pub <broker> <topic> <message>   (plaintext :1883)
 *   run rom/mqtt sub <broker> <topic> [seconds]
 *
 * TLS (pubs/subs, :8883) is not yet supported — it needs netd-side TLS.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include <l4/re/env>
#include <l4/util/util.h>
#include <l4/sys/kip.h>

#include <net_ipc.h>

namespace {

L4::Cap<Net_svr> g_netd;
l4_uint32_t      g_h = 0;

/* ---- MQTT packet encoding ---- */
int enc_rl(uint8_t *out, int len)
{
  int i = 0;
  do { uint8_t b = len % 128; len /= 128; if (len) b |= 0x80; out[i++] = b; }
  while (len);
  return i;
}
int dec_rl(const uint8_t *buf, int *pos, int end)
{
  int mult = 1, val = 0; uint8_t b;
  do {
    if (*pos >= end) return -1;
    b = buf[(*pos)++]; val += (b & 0x7f) * mult; mult *= 128;
  } while (b & 0x80);
  return val;
}

bool mq_send(const uint8_t *p, int n)
{
  l4_uint32_t sent = 0;
  return g_netd->send(g_h, L4::Ipc::Array<char const>(n, (char const *)p), sent)
         == L4_EOK;
}

/* One netd recv (bounded by netd's recv timeout); returns bytes (0 = none). */
int mq_recv(uint8_t *buf, int max)
{
  L4::Ipc::Array<char> a(max, (char *)buf);
  if (g_netd->recv(g_h, max, a) != L4_EOK) return 0;
  return a.length;
}

bool mqtt_connect(const char *client_id)
{
  uint8_t p[256];
  uint8_t var[] = { 0x00,0x04,'M','Q','T','T', 0x04, 0x02, 0x00,0x3c };
  int clen = strlen(client_id);
  int rl = sizeof(var) + 2 + clen;
  int i = 0;
  p[i++] = 0x10; i += enc_rl(p + i, rl);
  memcpy(p + i, var, sizeof(var)); i += sizeof(var);
  p[i++] = (clen >> 8) & 0xff; p[i++] = clen & 0xff;
  memcpy(p + i, client_id, clen); i += clen;
  if (!mq_send(p, i)) return false;

  uint8_t r[8];
  int n = mq_recv(r, sizeof(r));
  /* CONNACK = 0x20 0x02 <flags> <return code 0=accepted> */
  return n >= 4 && (r[0] >> 4) == 2 && r[3] == 0;
}

bool mqtt_publish(const char *topic, const char *msg)
{
  uint8_t p[1024];
  int tl = strlen(topic), ml = strlen(msg);
  int rl = 2 + tl + ml;                 /* QoS 0: no packet id */
  int i = 0;
  p[i++] = 0x30; i += enc_rl(p + i, rl);
  p[i++] = (tl >> 8) & 0xff; p[i++] = tl & 0xff;
  memcpy(p + i, topic, tl); i += tl;
  memcpy(p + i, msg, ml); i += ml;
  return mq_send(p, i);
}

bool mqtt_subscribe(const char *topic)
{
  uint8_t p[256];
  int tl = strlen(topic);
  int rl = 2 + (2 + tl + 1);
  int i = 0;
  p[i++] = 0x82; i += enc_rl(p + i, rl);
  p[i++] = 0x00; p[i++] = 0x01;          /* packet id */
  p[i++] = (tl >> 8) & 0xff; p[i++] = tl & 0xff;
  memcpy(p + i, topic, tl); i += tl;
  p[i++] = 0x00;                          /* requested QoS 0 */
  if (!mq_send(p, i)) return false;

  uint8_t r[8];
  int n = mq_recv(r, sizeof(r));
  return n >= 4 && (r[0] >> 4) == 9;      /* SUBACK */
}

void mqtt_disconnect()
{
  uint8_t p[2] = { 0xe0, 0x00 };
  mq_send(p, 2);
}

void mqtt_pingreq()
{
  uint8_t p[2] = { 0xc0, 0x00 };
  mq_send(p, 2);
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 4)
    {
      printf("usage: mqtt pub <broker> <topic> <message>   (plaintext :1883)\n"
             "       mqtt sub <broker> <topic> [seconds]\n");
      return 1;
    }

  const char *op = argv[1];
  if (strcmp(op, "pubs") == 0 || strcmp(op, "subs") == 0)
    {
      printf("mqtt: TLS (%s) not yet supported via netd — use pub/sub\n", op);
      return 1;
    }
  bool is_sub = (strcmp(op, "sub") == 0);
  if (!is_sub && strcmp(op, "pub") != 0)
    {
      printf("mqtt: unknown op '%s' (pub|sub)\n", op);
      return 1;
    }

  const char *broker = argv[2];
  const char *topic  = argv[3];

  g_netd = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!g_netd.is_valid()) { printf("mqtt: netd unavailable\n"); return 1; }

  l4_uint32_t ip_be = 0;
  if (g_netd->resolve(L4::Ipc::Array<char const>(strlen(broker), broker), ip_be)
      != L4_EOK)
    { printf("mqtt: can't resolve broker '%s'\n", broker); return 1; }

  if (g_netd->tcp_connect(ip_be, 1883, g_h) != L4_EOK)
    { printf("mqtt: can't connect to %s:1883\n", broker); return 1; }

  if (!mqtt_connect("turingos-mqtt"))
    { printf("mqtt: CONNECT refused\n"); g_netd->close(g_h); return 1; }
  printf("mqtt: connected to %s:1883\n", broker);

  if (!is_sub)
    {
      const char *msg = (argc > 4) ? argv[4] : "";
      if (mqtt_publish(topic, msg))
        printf("mqtt: published to '%s': %s\n", topic, msg);
      else
        printf("mqtt: publish failed\n");
    }
  else
    {
      if (!mqtt_subscribe(topic))
        { printf("mqtt: SUBSCRIBE failed\n"); mqtt_disconnect();
          g_netd->close(g_h); return 1; }
      int secs = (argc > 4) ? atoi(argv[4]) : 10;
      printf("mqtt: subscribed to '%s', listening %ds...\n", topic, secs);

      l4_uint64_t start = l4_kip_clock(l4re_kip());
      l4_uint64_t dur   = (l4_uint64_t)secs * 1000000;
      int polls = 0;
      while (l4_kip_clock(l4re_kip()) - start < dur)
        {
          uint8_t rb[1024];
          int got = mq_recv(rb, sizeof(rb));
          if (got == 0) { if (++polls % 15 == 0) mqtt_pingreq(); continue; }
          int pos = 0;
          while (pos < got)
            {
              uint8_t type = rb[pos] >> 4; pos++;
              int rl = dec_rl(rb, &pos, got);
              if (rl < 0 || pos + rl > got) break;
              if (type == 3)            /* PUBLISH */
                {
                  int tl = (rb[pos] << 8) | rb[pos + 1];
                  const char *t = (const char *)rb + pos + 2;
                  const char *pl = (const char *)rb + pos + 2 + tl;
                  int plen = rl - 2 - tl;     /* QoS 0: no packet id */
                  printf("mqtt: [%.*s] %.*s\n", tl, t, plen, pl);
                }
              pos += rl;
            }
        }
    }

  mqtt_disconnect();
  g_netd->close(g_h);
  return 0;
}
