/*
 * net-cluster — MQTT client (plaintext, QoS 0) over lwIP's mqtt app.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Runs in native_shell's process (which owns the lwIP stack).  lwIP mqtt
 * calls must hold the TCPIP core lock; the connection / publish / incoming
 * callbacks run on the tcpip thread, so we kick off an op then poll a flag.
 */

#include "nc_mqtt.h"

#include <lwip/apps/mqtt.h>
#include <lwip/tcpip.h>
#include <lwip/ip_addr.h>
#include <lwip/sys.h>
#include <lwip/altcp_tls.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* Exported by native_shell (cmd_net.cc); true once lwIP+virtio-net are up. */
bool net_is_ready();

namespace {

struct Mqtt_ctx {
  volatile int  conn_status;      // -2 = pending; else mqtt_connection_status_t
  volatile int  pub_done;
  volatile int  pub_err;
  char          cur_topic[128];   // last incoming PUBLISH topic (for sub)
};

void conn_cb(mqtt_client_t *, void *arg, mqtt_connection_status_t status)
{
  static_cast<Mqtt_ctx *>(arg)->conn_status = (int)status;
}

void req_cb(void *arg, err_t err)   // publish / subscribe completion
{
  Mqtt_ctx *x = static_cast<Mqtt_ctx *>(arg);
  x->pub_err  = (int)err;
  x->pub_done = 1;
}

void inpub_cb(void *arg, const char *topic, u32_t)
{
  Mqtt_ctx *x = static_cast<Mqtt_ctx *>(arg);
  strncpy(x->cur_topic, topic, sizeof(x->cur_topic) - 1);
  x->cur_topic[sizeof(x->cur_topic) - 1] = '\0';
}

void indata_cb(void *arg, const u8_t *data, u16_t len, u8_t /*flags*/)
{
  Mqtt_ctx *x = static_cast<Mqtt_ctx *>(arg);
  printf("mqtt: %s: %.*s\n", x->cur_topic, (int)len, (const char *)data);
}

} // namespace

void cmd_mqtt(int argc, char **argv)
{
  if (!net_is_ready()) { printf("mqtt: network not ready\n"); return; }
  if (argc < 4) {
    printf("usage: mqtt pub  <broker_ip> <topic> <message>   (plaintext :1883)\n"
           "       mqtt sub  <broker_ip> <topic> [seconds]\n"
           "       mqtt pubs <broker_ip> <topic> <message>   (TLS :8883)\n"
           "       mqtt subs <broker_ip> <topic> [seconds]\n");
    return;
  }
  /* pubs/subs = TLS variants of pub/sub (port 8883). */
  char op[8];
  strncpy(op, argv[1], sizeof(op) - 1);
  op[sizeof(op) - 1] = '\0';
  bool tls = (strcmp(op, "pubs") == 0 || strcmp(op, "subs") == 0);
  if (tls) op[strlen(op) - 1] = '\0';      // pubs->pub, subs->sub
  const u16_t port = tls ? 8883 : 1883;

  const char *broker = argv[2];
  const char *topic  = argv[3];

  ip_addr_t bip;
  if (!ipaddr_aton(broker, &bip)) {
    printf("mqtt: bad broker IP '%s'\n", broker);
    return;
  }

  static Mqtt_ctx ctx;                  // outlives the tcpip-thread callbacks
  ctx.conn_status = -2;
  ctx.pub_done    = 0;
  ctx.pub_err     = 0;
  ctx.cur_topic[0] = '\0';

  struct mqtt_connect_client_info_t info;
  memset(&info, 0, sizeof(info));
  info.client_id  = "turingos";
  info.keep_alive = 60;

  /* TLS: a no-verify client config (CA=NULL) for the first cut — proves the
   * handshake; certificate verification is a follow-up.  The config owns the
   * entropy/DRBG context (expensive to set up) and must outlive every
   * connection that references it; altcp_tls's intended use is a long-lived
   * config.  So create it ONCE and keep it for the process lifetime — never
   * free it (freeing it while a TLS close is still in flight crashes).
   * altcp_tls_create_config_client mem_mallocs via lwIP — hold the core lock. */
  static struct altcp_tls_config *s_tlscfg = nullptr;
  if (tls) {
    if (!s_tlscfg) {
      LOCK_TCPIP_CORE();
      s_tlscfg = altcp_tls_create_config_client(nullptr, 0);
      UNLOCK_TCPIP_CORE();
    }
    if (!s_tlscfg) { printf("mqtt: TLS config alloc failed\n"); return; }
    info.tls_config = s_tlscfg;
  }

  /* Every lwIP mqtt_* call asserts the TCPIP core lock is held; only the
   * poll-sleeps below run unlocked (holding it would stall the tcpip thread). */
  printf("mqtt: connecting to %s:%d %s...\n", broker, port, tls ? "(TLS) " : "");
  LOCK_TCPIP_CORE();
  mqtt_client_t *cl = mqtt_client_new();
  err_t e = ERR_MEM;
  if (cl) {
    e = mqtt_client_connect(cl, &bip, port, conn_cb, &ctx, &info);
    if (strcmp(op, "sub") == 0)
      mqtt_set_inpub_callback(cl, inpub_cb, indata_cb, &ctx);
  }
  UNLOCK_TCPIP_CORE();

  if (!cl) {
    printf("mqtt: client alloc failed\n");
    return;
  }
  if (e != ERR_OK) {
    printf("mqtt: connect call failed (%d)\n", (int)e);
    LOCK_TCPIP_CORE(); mqtt_client_free(cl); UNLOCK_TCPIP_CORE();
    return;
  }

  /* Wait for the CONNACK (callback on tcpip thread). TLS handshake adds a few
   * round-trips, so allow longer than the plaintext path. */
  const int conn_timeout = tls ? 12000 : 5000;
  for (int waited = 0; ctx.conn_status == -2 && waited < conn_timeout; waited += 100)
    sys_msleep(100);
  if (ctx.conn_status != MQTT_CONNECT_ACCEPTED) {
    printf("mqtt: connect failed (status=%d)\n", ctx.conn_status);
    LOCK_TCPIP_CORE(); mqtt_disconnect(cl); mqtt_client_free(cl); UNLOCK_TCPIP_CORE();
    return;
  }
  printf("mqtt: connected%s\n", tls ? " (TLS)" : "");

  if (strcmp(op, "pub") == 0) {
    const char *msg = (argc > 4) ? argv[4] : "";
    LOCK_TCPIP_CORE();
    e = mqtt_publish(cl, topic, msg, (u16_t)strlen(msg), 0, 0, req_cb, &ctx);
    UNLOCK_TCPIP_CORE();
    if (e != ERR_OK) {
      printf("mqtt: publish call failed (%d)\n", (int)e);
    } else {
      for (int waited = 0; !ctx.pub_done && waited < 3000; waited += 100)
        sys_msleep(100);
      if (ctx.pub_done && ctx.pub_err == ERR_OK)
        printf("mqtt: published to %s\n", topic);
      else
        printf("mqtt: publish %s (err=%d)\n",
               ctx.pub_done ? "failed" : "timeout", ctx.pub_err);
    }
  } else if (strcmp(op, "sub") == 0) {
    int secs = (argc > 4) ? atoi(argv[4]) : 10;
    if (secs <= 0) secs = 10;
    LOCK_TCPIP_CORE();
    e = mqtt_subscribe(cl, topic, 0, req_cb, &ctx);
    UNLOCK_TCPIP_CORE();
    if (e != ERR_OK) {
      printf("mqtt: subscribe call failed (%d)\n", (int)e);
    } else {
      printf("mqtt: subscribed to %s, listening %ds ...\n", topic, secs);
      for (int i = 0; i < secs * 10; i++)
        sys_msleep(100);
    }
  } else {
    printf("mqtt: unknown op '%s' (use pub or sub)\n", op);
  }

  LOCK_TCPIP_CORE(); mqtt_disconnect(cl); mqtt_client_free(cl); UNLOCK_TCPIP_CORE();
}
