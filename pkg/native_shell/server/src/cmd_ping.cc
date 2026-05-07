/*
 * cmd_ping.cc — ping, nslookup, udp commands for native_shell
 *
 * ping    <host> [-c N]  — ICMP echo using raw socket (LWIP_RAW=1)
 * nslookup <host>        — DNS A-record lookup via lwip_getaddrinfo
 * udp     [echo [port]]  — background UDP echo server on port 5001
 *
 * QEMU NAT addresses used:
 *   gateway  10.0.2.2
 *   DNS      10.0.2.3
 *   UDP fwd  hostfwd=udp::5556-:5001  (added to run_qemu_virt.sh)
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

#include <lwip/tcpip.h>
#include <lwip/sockets.h>
#include <lwip/ip4_addr.h>
#include <lwip/prot/icmp.h>
#include <lwip/prot/ip4.h>
#include <lwip/netdb.h>

#include <l4/util/util.h>
#include <pthread.h>
#include <pthread-l4.h>

#include "commands.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static uint16_t icmp_chksum(const void *data, size_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Resolve a dotted-decimal or hostname string to ip4_addr_t.
   Returns true on success. */
static bool resolve_host(const char *host, ip4_addr_t *out, char *resolved_str, int rlen)
{
    if (ip4addr_aton(host, out)) {
        snprintf(resolved_str, (size_t)rlen, "%s", ip4addr_ntoa(out));
        return true;
    }

    struct addrinfo hints = {};
    struct addrinfo *res  = nullptr;
    hints.ai_family = AF_INET;
    if (lwip_getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        if (res) lwip_freeaddrinfo(res);
        return false;
    }
    auto *sin  = (struct sockaddr_in *)res->ai_addr;
    out->addr  = sin->sin_addr.s_addr;
    snprintf(resolved_str, (size_t)rlen, "%s", ip4addr_ntoa(out));
    lwip_freeaddrinfo(res);
    return true;
}

/* ------------------------------------------------------------------ */
/* ping                                                                 */
/* ------------------------------------------------------------------ */

#define PING_DATA_SIZE  56u
#define PING_PKT_SIZE   (sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE)
#define PING_ID         0xABCDu

void cmd_ping(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: ping <host> [-c count]\n");
        printf("  ping 10.0.2.2        — ping QEMU gateway\n");
        printf("  ping 10.0.2.2 -c 10  — send 10 pings\n");
        return;
    }

    if (!net_is_ready()) {
        klog_warn(KLOG_NET, "ping: network not ready");
        printf("ping: network not ready\n");
        return;
    }

    const char *host = argv[1];
    int count = 4;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
    }

    ip4_addr_t target;
    char target_str[16];
    if (!resolve_host(host, &target, target_str, sizeof(target_str))) {
        klog_warn(KLOG_NET, "ping: cannot resolve '%s'", host);
        printf("ping: cannot resolve '%s'\n", host);
        return;
    }

    printf("PING %s (%s): %u data bytes\n", host, target_str, PING_DATA_SIZE);

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) { perror("ping: socket"); return; }

    struct timeval tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[PING_PKT_SIZE];
    int sent = 0, rcvd = 0;
    long min_us = 999999999L, max_us = 0, total_us = 0;

    for (int seq = 0; seq < count && !g_shell_interrupt; seq++) {
        /* Build ICMP echo request */
        memset(pkt, 0, sizeof(pkt));
        auto *hdr = (struct icmp_echo_hdr *)pkt;
        ICMPH_TYPE_SET(hdr, ICMP_ECHO);
        ICMPH_CODE_SET(hdr, 0);
        hdr->id    = htons(PING_ID);
        hdr->seqno = htons((uint16_t)seq);
        for (size_t i = sizeof(*hdr); i < PING_PKT_SIZE; i++)
            pkt[i] = (uint8_t)(i - sizeof(*hdr));
        hdr->chksum = icmp_chksum(pkt, PING_PKT_SIZE);

        struct sockaddr_in dst{};
        dst.sin_family      = AF_INET;
        dst.sin_addr.s_addr = target.addr;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (sendto(s, pkt, PING_PKT_SIZE, 0,
                   (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            klog_warn(KLOG_NET, "ping: send error: %s", strerror(errno));
            printf("ping: send error: %s\n", strerror(errno));
            continue;
        }
        sent++;

        /* Wait for ICMP echo reply */
        uint8_t buf[256];
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t rn = recvfrom(s, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &fromlen);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        if (rn < 0) {
            printf("Request timeout for icmp_seq %d\n", seq);
            continue;
        }

        /* Parse IP header + ICMP echo reply */
        if (rn < (ssize_t)sizeof(struct ip_hdr)) continue;
        auto *ip    = (struct ip_hdr *)buf;
        int   iphlen = IPH_HL(ip) * 4;
        if (rn < iphlen + (int)sizeof(struct icmp_echo_hdr)) continue;

        auto *reply = (struct icmp_echo_hdr *)(buf + iphlen);
        if (ICMPH_TYPE(reply) != ICMP_ER) continue;
        if (ntohs(reply->id) != PING_ID) continue; /* discard stale or other apps' replies */

        long rtt_us = (t1.tv_sec  - t0.tv_sec)  * 1000000L
                    + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        rcvd++;
        if (rtt_us < min_us) min_us = rtt_us;
        if (rtt_us > max_us) max_us = rtt_us;
        total_us += rtt_us;

        printf("%zd bytes from %s: icmp_seq=%d ttl=%d time=%ld.%ld ms\n",
               rn - iphlen,
               inet_ntoa(from.sin_addr),
               ntohs(reply->seqno),
               (int)IPH_TTL(ip),
               rtt_us / 1000, (rtt_us % 1000) / 100);

        if (seq < count - 1 && !g_shell_interrupt)
            l4_sleep(1000);
    }

    close(s);
    printf("\n--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           sent, rcvd, sent ? (sent - rcvd) * 100 / sent : 0);
    if (rcvd > 0)
        printf("rtt min/avg/max = %ld.%ld/%ld.%ld/%ld.%ld ms\n",
               min_us / 1000,             (min_us % 1000) / 100,
               (total_us / rcvd) / 1000,  ((total_us / rcvd) % 1000) / 100,
               max_us / 1000,             (max_us % 1000) / 100);
}

/* ------------------------------------------------------------------ */
/* nslookup                                                             */
/* ------------------------------------------------------------------ */

void cmd_nslookup(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: nslookup <hostname>\n");
        return;
    }

    if (!net_is_ready()) {
        klog_warn(KLOG_NET, "nslookup: network not ready");
        printf("nslookup: network not ready\n");
        return;
    }

    const char *host = argv[1];
    printf("Server: 10.0.2.3\n\n");
    printf("Name:   %s\n", host);

    struct addrinfo hints = {};
    struct addrinfo *res  = nullptr;
    hints.ai_family = AF_UNSPEC;

    int r = lwip_getaddrinfo(host, nullptr, &hints, &res);
    if (r != 0) {
        klog_warn(KLOG_NET, "nslookup: failed to resolve '%s' (err=%d)", host, r);
        printf("nslookup: failed to resolve '%s' (err=%d)\n", host, r);
        return;
    }

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            auto *sin = (struct sockaddr_in *)ai->ai_addr;
            printf("Address: %s\n", inet_ntoa(sin->sin_addr));
        }
    }
    lwip_freeaddrinfo(res);
}

/* ------------------------------------------------------------------ */
/* UDP echo server                                                      */
/* ------------------------------------------------------------------ */

#define UDP_ECHO_PORT  5001

static volatile bool g_udp_running = false;

static void *udp_echo_thread(void * /*arg*/)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        perror("udp: socket");
        g_udp_running = false;
        return nullptr;
    }

    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(UDP_ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("udp: bind");
        close(s);
        g_udp_running = false;
        return nullptr;
    }

    klog_info(KLOG_NET, "udp: echo server on port %d", UDP_ECHO_PORT);
    printf("udp: echo server listening on port %d\n", UDP_ECHO_PORT);
    printf("udp: test with: echo 'hello' | nc -u localhost 5556\n");

    task_register("udp-echo", "UDP echo server on port 5001");

    char buf[1024];
    for (;;) {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&cli, &clen);
        if (n < 0) {
            perror("udp: recvfrom");
            break;
        }
        buf[n] = '\0';
        printf("udp: %zd bytes from %s:%d: %.*s",
               n, inet_ntoa(cli.sin_addr), ntohs(cli.sin_port),
               (int)n, buf);
        if (n > 0 && buf[n - 1] != '\n') putchar('\n');
        sendto(s, buf, (size_t)n, 0, (struct sockaddr *)&cli, clen);
    }

    close(s);
    task_unregister("udp-echo");
    g_udp_running = false;
    return nullptr;
}

void cmd_udp(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!net_is_ready()) {
        printf("udp: network not ready\n");
        return;
    }

    if (g_udp_running) {
        printf("udp: echo server already running on port %d\n", UDP_ECHO_PORT);
        return;
    }

    g_udp_running = true;

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, udp_echo_thread, nullptr) != 0) {
        perror("udp: pthread_create");
        g_udp_running = false;
    }
    pthread_attr_destroy(&attr);
}
