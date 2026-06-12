/*
 * netd — the TuringOS network server.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Phase 1 of decomposing native_shell's in-process network stack into an
 * isolated server.  netd owns:
 *   - the virtio-net NIC, mapped via the io-managed **vbus** (NOT sigma0 — so
 *     netd never gets raw physical memory access), plus its DMA rings;
 *   - the whole lwIP TCP/IP stack (tcpip thread started by lwip's l4libinit
 *     constructor before main) + DHCP/static IP config.
 * It exposes a minimal TCP-client socket interface over IPC (Net_svr,
 * net_ipc.h): tcp_connect / send / recv / close.  Clients use the network
 * purely through this gate, so they no longer need lwIP or sigma0.
 *
 * The virtio-mmio driver, lwIP netif glue and DHCP code below are ported
 * essentially verbatim from native_shell's cmd_net.cc (already proven on QEMU
 * virt); the only substantive change is net_map_mmio(), which now discovers
 * and maps the NIC through the vbus instead of l4sigma0_map_iomem().
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>
#include <set>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <lwip/tcpip.h>
#include <lwip/netifapi.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>
#include <lwip/prot/icmp.h>
#include <lwip/prot/ip4.h>
#include <netif/ethernet.h>

#include <sys/time.h>
#include <ctime>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/dma_space>
#include <l4/re/mem_alloc>
#include <l4/re/namespace>
#include <l4/util/util.h>
#include <l4/re/rm>
#include <l4/re/util/unique_cap>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/sys/cxx/ipc_epiface>
#include <l4/sys/consts.h>
#include <l4/vbus/vbus>

#include <pthread.h>
#include <pthread-l4.h>

#include <net_ipc.h>

/* ------------------------------------------------------------------ */
/* Virtio-MMIO register offsets                                        */
/* ------------------------------------------------------------------ */
#define VTMMIO_MAGIC          0x000
#define VTMMIO_VERSION        0x004
#define VTMMIO_DEVICE_ID      0x008
#define VTMMIO_VENDOR_ID      0x00c
#define VTMMIO_DEV_FEAT       0x010
#define VTMMIO_DEV_FEAT_SEL   0x014
#define VTMMIO_DRV_FEAT       0x020
#define VTMMIO_DRV_FEAT_SEL   0x024
#define VTMMIO_QUEUE_SEL      0x030
#define VTMMIO_QUEUE_NUM_MAX  0x034
#define VTMMIO_QUEUE_NUM      0x038
#define VTMMIO_QUEUE_READY    0x044
#define VTMMIO_QUEUE_NOTIFY   0x050
#define VTMMIO_IRQ_STATUS     0x060
#define VTMMIO_IRQ_ACK        0x064
#define VTMMIO_STATUS         0x070
#define VTMMIO_QUEUE_DESC_LO  0x080
#define VTMMIO_QUEUE_DESC_HI  0x084
#define VTMMIO_QUEUE_DRV_LO   0x090
#define VTMMIO_QUEUE_DRV_HI   0x094
#define VTMMIO_QUEUE_DEV_LO   0x0a0
#define VTMMIO_QUEUE_DEV_HI   0x0a4
#define VTMMIO_CONFIG         0x100

/* Legacy (v1) extra */
#define VTMMIO_GUEST_PAGE_SIZE 0x028
#define VTMMIO_QUEUE_ALIGN     0x03c
#define VTMMIO_QUEUE_PFN       0x040

#define QUEUE_ALIGN_V1  64U
#define Q_USED_OFF      320U

/* On-wire virtio-net header size in legacy mode (v1) without
 * VIRTIO_NET_F_MRG_RXBUF: 10 bytes, no num_buffers field. */
#define VNET_HDR_WIRE   10

/* Device status bits */
#define VTSTS_ACKNOWLEDGE     0x01
#define VTSTS_DRIVER          0x02
#define VTSTS_DRIVER_OK       0x04
#define VTSTS_FEATURES_OK     0x08
#define VTSTS_FAILED          0x80

/* Device IDs / features */
#define VTDEV_NET             1
#define VTNET_F_MAC           (1u << 5)
#define VT_F_VERSION_1        (1u << 0)

/* Descriptor flags */
#define VTDESC_F_NEXT         0x0001
#define VTDESC_F_WRITE        0x0002

#define VQ_SIZE               16

/* ------------------------------------------------------------------ */
/* Data structures                                                      */
/* ------------------------------------------------------------------ */
struct __attribute__((packed)) vnet_hdr {
  l4_uint8_t  flags;
  l4_uint8_t  gso_type;
  l4_uint16_t hdr_len;
  l4_uint16_t gso_size;
  l4_uint16_t csum_start;
  l4_uint16_t csum_offset;
  l4_uint16_t num_buffers;
};

struct __attribute__((packed)) vq_desc {
  l4_uint64_t addr;
  l4_uint32_t len;
  l4_uint16_t flags;
  l4_uint16_t next;
};

struct __attribute__((packed)) vq_avail {
  l4_uint16_t flags;
  l4_uint16_t idx;
  l4_uint16_t ring[VQ_SIZE];
  l4_uint16_t used_event;
};

struct __attribute__((packed)) vq_used_elem {
  l4_uint32_t id;
  l4_uint32_t len;
};

struct __attribute__((packed)) vq_used {
  l4_uint16_t   flags;
  l4_uint16_t   idx;
  vq_used_elem  ring[VQ_SIZE];
  l4_uint16_t   avail_event;
};

struct __attribute__((packed, aligned(4096))) virtnet_q {
  vq_desc  desc[VQ_SIZE];
  vq_avail avail;
  l4_uint8_t _pad[Q_USED_OFF
    - sizeof(vq_desc) * VQ_SIZE
    - sizeof(vq_avail)];
  vq_used  used;
  l4_uint8_t _tail[L4_PAGESIZE - Q_USED_OFF - sizeof(vq_used)];
};

struct __attribute__((packed, aligned(4096))) virtnet_dma {
  virtnet_q  rx_q;
  virtnet_q  tx_q;
  l4_uint8_t rx_buf[VQ_SIZE][2048];
  l4_uint8_t tx_buf[2048];
};

/* ------------------------------------------------------------------ */
/* Driver state (file-scoped)                                           */
/* ------------------------------------------------------------------ */
static volatile l4_uint8_t *n_mmio_base = nullptr;
static struct virtnet_dma   *n_dma      = nullptr;
static l4_uint64_t           n_dma_phys = 0;

static l4_uint16_t n_rx_last_used = 0;
static l4_uint16_t n_tx_last_used = 0;
static l4_uint16_t n_tx_avail_idx = 0;

static struct netif n_netif;

static bool g_net_stack_ready = false; /* true once hw+lwIP init done */

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                         */
/* ------------------------------------------------------------------ */
static inline l4_uint32_t nreg_r(unsigned off)
{ return *reinterpret_cast<volatile l4_uint32_t const *>(n_mmio_base + off); }

static inline void nreg_w(unsigned off, l4_uint32_t v)
{ *reinterpret_cast<volatile l4_uint32_t *>(n_mmio_base + off) = v; }

/* ------------------------------------------------------------------ */
/* lwIP netif TX callback                                               */
/* ------------------------------------------------------------------ */
static err_t net_output(struct netif * /*netif*/, struct pbuf *p)
{
  l4_uint16_t pkt_len = (l4_uint16_t)(VNET_HDR_WIRE + p->tot_len);
  if (pkt_len > (l4_uint16_t)sizeof(n_dma->tx_buf))
    return ERR_MEM;

  /* Zero the 10-byte on-wire header (no num_buffers in legacy mode) */
  memset(n_dma->tx_buf, 0, VNET_HDR_WIRE);

  l4_uint8_t *payload = n_dma->tx_buf + VNET_HDR_WIRE;
  pbuf_copy_partial(p, payload, p->tot_len, 0);

  l4_uint16_t slot = n_tx_avail_idx & (VQ_SIZE - 1);

  n_dma->tx_q.desc[slot].addr  = n_dma_phys
    + __builtin_offsetof(struct virtnet_dma, tx_buf);
  n_dma->tx_q.desc[slot].len   = pkt_len;
  n_dma->tx_q.desc[slot].flags = 0;
  n_dma->tx_q.desc[slot].next  = 0;

  n_dma->tx_q.avail.ring[n_tx_avail_idx & (VQ_SIZE - 1)] = slot;
  __sync_synchronize();
  n_dma->tx_q.avail.idx = (l4_uint16_t)(n_tx_avail_idx + 1);
  __sync_synchronize();

  nreg_w(VTMMIO_QUEUE_NOTIFY, 1);
  ++n_tx_avail_idx;

  while (n_dma->tx_q.used.idx == n_tx_last_used)
    ;

  nreg_w(VTMMIO_IRQ_ACK, nreg_r(VTMMIO_IRQ_STATUS));
  while (n_tx_last_used != n_dma->tx_q.used.idx)
    ++n_tx_last_used;

  return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* RX polling thread                                                    */
/* ------------------------------------------------------------------ */
static void *net_rx_thread(void *arg)
{
  struct netif *nif = reinterpret_cast<struct netif *>(arg);

  for (;;) {
    while (n_dma->rx_q.used.idx == n_rx_last_used)
      l4_sleep(10);

    nreg_w(VTMMIO_IRQ_ACK, nreg_r(VTMMIO_IRQ_STATUS));

    while (n_rx_last_used != n_dma->rx_q.used.idx) {
      vq_used_elem &ue = n_dma->rx_q.used.ring[n_rx_last_used & (VQ_SIZE - 1)];
      l4_uint16_t   id  = (l4_uint16_t)ue.id;
      l4_uint32_t   len = ue.len;

      if (len > VNET_HDR_WIRE) {
        l4_uint8_t *frame = n_dma->rx_buf[id] + VNET_HDR_WIRE;
        l4_uint16_t flen  = (l4_uint16_t)(len - VNET_HDR_WIRE);

        struct pbuf *pb = pbuf_alloc(PBUF_RAW, flen, PBUF_POOL);
        if (pb) {
          pbuf_take(pb, frame, flen);
          LOCK_TCPIP_CORE();
          ethernet_input(pb, nif);
          UNLOCK_TCPIP_CORE();
        }
      }

      /* Re-arm descriptor */
      n_dma->rx_q.desc[id].addr  = n_dma_phys
        + __builtin_offsetof(struct virtnet_dma, rx_buf)
        + (l4_uint64_t)id * 2048;
      n_dma->rx_q.desc[id].len   = 2048;
      n_dma->rx_q.desc[id].flags = VTDESC_F_WRITE;
      n_dma->rx_q.desc[id].next  = 0;

      __sync_synchronize();
      n_dma->rx_q.avail.ring[n_dma->rx_q.avail.idx & (VQ_SIZE - 1)] = id;
      n_dma->rx_q.avail.idx = (l4_uint16_t)(n_dma->rx_q.avail.idx + 1);
      __sync_synchronize();
      nreg_w(VTMMIO_QUEUE_NOTIFY, 0);

      ++n_rx_last_used;
    }
  }
  return nullptr;
}

/* ------------------------------------------------------------------ */
/* lwIP netif init callback                                             */
/* ------------------------------------------------------------------ */
static err_t net_netif_init(struct netif *nif)
{
  nif->name[0]    = 'v';
  nif->name[1]    = 'n';
  nif->output     = etharp_output;
  nif->linkoutput = net_output;
  nif->mtu        = 1500;
  nif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                  | NETIF_FLAG_LINK_UP   | NETIF_FLAG_UP;

  for (int i = 0; i < 6; ++i)
    nif->hwaddr[i] = *reinterpret_cast<volatile l4_uint8_t const *>(
                       n_mmio_base + VTMMIO_CONFIG + i);
  nif->hwaddr_len = 6;

  printf("[netd] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
         nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
         nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5]);

  netif_set_default(nif);

  pthread_t t;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&t, &attr, net_rx_thread, nif);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    printf("[netd] failed to create RX thread\n");
    return ERR_IF;
  }
  return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Virtio init helpers                                                  */
/* ------------------------------------------------------------------ */
static void net_prefill_rx(l4_uint32_t qsz)
{
  l4_uint64_t buf_base = n_dma_phys
    + __builtin_offsetof(struct virtnet_dma, rx_buf);
  for (l4_uint32_t i = 0; i < qsz; ++i) {
    n_dma->rx_q.desc[i].addr  = buf_base + (l4_uint64_t)i * 2048;
    n_dma->rx_q.desc[i].len   = 2048;
    n_dma->rx_q.desc[i].flags = VTDESC_F_WRITE;
    n_dma->rx_q.desc[i].next  = 0;
    n_dma->rx_q.avail.ring[i] = (l4_uint16_t)i;
  }
  n_dma->rx_q.avail.flags = 0;
  n_dma->rx_q.avail.idx   = (l4_uint16_t)qsz;
  __sync_synchronize();
  nreg_w(VTMMIO_QUEUE_NOTIFY, 0);
}

static bool net_virtio_init_v1()
{
  printf("[netd] legacy protocol (v1)\n");
  nreg_w(VTMMIO_GUEST_PAGE_SIZE, L4_PAGESIZE);
  nreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER);
  __sync_synchronize();

  l4_uint32_t feat = nreg_r(VTMMIO_DEV_FEAT) & VTNET_F_MAC;
  nreg_w(VTMMIO_DRV_FEAT, feat);

  nreg_w(VTMMIO_QUEUE_SEL, 0);
  l4_uint32_t qmax = nreg_r(VTMMIO_QUEUE_NUM_MAX);
  l4_uint32_t qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  nreg_w(VTMMIO_QUEUE_NUM, qsz);
  nreg_w(VTMMIO_QUEUE_ALIGN, QUEUE_ALIGN_V1);
  l4_uint32_t rx_pfn = (l4_uint32_t)(
    (n_dma_phys + __builtin_offsetof(struct virtnet_dma, rx_q)) >> L4_PAGESHIFT);
  nreg_w(VTMMIO_QUEUE_PFN, rx_pfn);

  nreg_w(VTMMIO_QUEUE_SEL, 1);
  qmax = nreg_r(VTMMIO_QUEUE_NUM_MAX);
  qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  nreg_w(VTMMIO_QUEUE_NUM, qsz);
  nreg_w(VTMMIO_QUEUE_ALIGN, QUEUE_ALIGN_V1);
  l4_uint32_t tx_pfn = (l4_uint32_t)(
    (n_dma_phys + __builtin_offsetof(struct virtnet_dma, tx_q)) >> L4_PAGESHIFT);
  nreg_w(VTMMIO_QUEUE_PFN, tx_pfn);

  n_dma->tx_q.avail.flags = 0;
  n_dma->tx_q.avail.idx   = 0;

  /* DRIVER_OK must come before prefill so QEMU processes QUEUE_NOTIFY(0)
     while the device is active. */
  nreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_DRIVER_OK);
  __sync_synchronize();

  net_prefill_rx(qsz);   /* fills avail ring and sends QUEUE_NOTIFY(0) */

  printf("[netd] device ready (v1 legacy)\n");
  return true;
}

static bool net_virtio_init_v2()
{
  printf("[netd] modern protocol (v2)\n");
  nreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER);

  nreg_w(VTMMIO_DEV_FEAT_SEL, 0);
  l4_uint32_t feat0 = nreg_r(VTMMIO_DEV_FEAT) & VTNET_F_MAC;
  nreg_w(VTMMIO_DEV_FEAT_SEL, 1);
  l4_uint32_t feat1 = nreg_r(VTMMIO_DEV_FEAT) & VT_F_VERSION_1;
  if (!(feat1 & VT_F_VERSION_1)) {
    printf("[netd] device does not support VERSION_1\n");
    return false;
  }
  nreg_w(VTMMIO_DRV_FEAT_SEL, 0); nreg_w(VTMMIO_DRV_FEAT, feat0);
  nreg_w(VTMMIO_DRV_FEAT_SEL, 1); nreg_w(VTMMIO_DRV_FEAT, feat1);

  nreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_FEATURES_OK);
  __sync_synchronize();
  if (!(nreg_r(VTMMIO_STATUS) & VTSTS_FEATURES_OK)) {
    printf("[netd] FEATURES_OK rejected\n");
    return false;
  }

  nreg_w(VTMMIO_QUEUE_SEL, 0);
  l4_uint32_t qmax = nreg_r(VTMMIO_QUEUE_NUM_MAX);
  l4_uint32_t qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  nreg_w(VTMMIO_QUEUE_NUM, qsz);
  l4_uint64_t rx_desc  = n_dma_phys + __builtin_offsetof(virtnet_dma, rx_q)
                         + __builtin_offsetof(virtnet_q, desc);
  l4_uint64_t rx_avail = n_dma_phys + __builtin_offsetof(virtnet_dma, rx_q)
                         + __builtin_offsetof(virtnet_q, avail);
  l4_uint64_t rx_used  = n_dma_phys + __builtin_offsetof(virtnet_dma, rx_q)
                         + Q_USED_OFF;
  nreg_w(VTMMIO_QUEUE_DESC_LO, (l4_uint32_t)(rx_desc  & 0xffffffff));
  nreg_w(VTMMIO_QUEUE_DESC_HI, (l4_uint32_t)(rx_desc  >> 32));
  nreg_w(VTMMIO_QUEUE_DRV_LO,  (l4_uint32_t)(rx_avail & 0xffffffff));
  nreg_w(VTMMIO_QUEUE_DRV_HI,  (l4_uint32_t)(rx_avail >> 32));
  nreg_w(VTMMIO_QUEUE_DEV_LO,  (l4_uint32_t)(rx_used  & 0xffffffff));
  nreg_w(VTMMIO_QUEUE_DEV_HI,  (l4_uint32_t)(rx_used  >> 32));
  nreg_w(VTMMIO_QUEUE_READY, 1);

  nreg_w(VTMMIO_QUEUE_SEL, 1);
  qmax = nreg_r(VTMMIO_QUEUE_NUM_MAX);
  qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  nreg_w(VTMMIO_QUEUE_NUM, qsz);
  l4_uint64_t tx_desc  = n_dma_phys + __builtin_offsetof(virtnet_dma, tx_q)
                         + __builtin_offsetof(virtnet_q, desc);
  l4_uint64_t tx_avail = n_dma_phys + __builtin_offsetof(virtnet_dma, tx_q)
                         + __builtin_offsetof(virtnet_q, avail);
  l4_uint64_t tx_used  = n_dma_phys + __builtin_offsetof(virtnet_dma, tx_q)
                         + Q_USED_OFF;
  nreg_w(VTMMIO_QUEUE_DESC_LO, (l4_uint32_t)(tx_desc  & 0xffffffff));
  nreg_w(VTMMIO_QUEUE_DESC_HI, (l4_uint32_t)(tx_desc  >> 32));
  nreg_w(VTMMIO_QUEUE_DRV_LO,  (l4_uint32_t)(tx_avail & 0xffffffff));
  nreg_w(VTMMIO_QUEUE_DRV_HI,  (l4_uint32_t)(tx_avail >> 32));
  nreg_w(VTMMIO_QUEUE_DEV_LO,  (l4_uint32_t)(tx_used  & 0xffffffff));
  nreg_w(VTMMIO_QUEUE_DEV_HI,  (l4_uint32_t)(tx_used  >> 32));
  nreg_w(VTMMIO_QUEUE_READY, 1);

  net_prefill_rx(qsz);
  n_dma->tx_q.avail.flags = 0;
  n_dma->tx_q.avail.idx   = 0;

  nreg_w(VTMMIO_STATUS,
         VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_FEATURES_OK | VTSTS_DRIVER_OK);
  __sync_synchronize();
  printf("[netd] device ready (v2 modern)\n");
  return true;
}

static bool net_virtio_init(l4_addr_t mmio_virt, l4_uint64_t phys_base,
                            struct virtnet_dma *vdma)
{
  n_mmio_base = reinterpret_cast<volatile l4_uint8_t *>(mmio_virt);
  n_dma       = vdma;
  n_dma_phys  = phys_base;

  l4_uint32_t magic   = nreg_r(VTMMIO_MAGIC);
  l4_uint32_t version = nreg_r(VTMMIO_VERSION);
  l4_uint32_t devid   = nreg_r(VTMMIO_DEVICE_ID);

  if (magic != 0x74726976u) {
    printf("[netd] bad magic 0x%08x\n", magic);
    return false;
  }
  if (devid != VTDEV_NET) {
    printf("[netd] device_id=%u, expected %u\n", devid, VTDEV_NET);
    return false;
  }
  if (version != 1 && version != 2) {
    printf("[netd] unsupported version %u\n", version);
    return false;
  }

  printf("[netd] found virtio-net device (v%u)\n", version);
  nreg_w(VTMMIO_STATUS, 0);
  __sync_synchronize();

  return (version == 1) ? net_virtio_init_v1() : net_virtio_init_v2();
}

/* ------------------------------------------------------------------ */
/* MMIO mapping via vbus (NOT sigma0)                                   */
/* ------------------------------------------------------------------ */
/* Discover the virtio-net device on the io-managed vbus and map its MMIO
 * registers (uncached).  This is the one substantive departure from
 * cmd_net.cc: netd never touches sigma0 / raw physical memory — it only sees
 * the single NIC the io manager hands it on vbus_net. */
static bool net_map_mmio(l4_addr_t *virt_out)
{
  auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (!vbus.is_valid()) {
    printf("[netd] 'vbus' cap not found\n");
    return false;
  }

  L4vbus::Device dev;
  l4vbus_device_t di;
  while (vbus->root().next_device(&dev, L4VBUS_MAX_DEPTH, &di) == L4_EOK) {
    if (dev.is_compatible("virtio,mmio") != 1)
      continue;

    l4_addr_t mmio_start = 0;
    l4_size_t mmio_size  = 0;
    bool found_mmio = false;
    for (unsigned i = 0; i < di.num_resources; ++i) {
      l4vbus_resource_t res;
      if (dev.get_resource(i, &res) != L4_EOK)
        continue;
      if (res.type == L4VBUS_RESOURCE_MEM && !found_mmio) {
        mmio_start = res.start;
        mmio_size  = res.end - res.start + 1;
        found_mmio = true;
      }
    }
    if (!found_mmio)
      continue;

    /* attach() snaps to the page boundary; keep the sub-page offset so the
     * register pointer lands on the actual device registers. */
    l4_addr_t page_offset = mmio_start & (L4_PAGESIZE - 1);
    auto iods =
      L4::Ipc::make_cap_rw(L4::cap_reinterpret_cast<L4Re::Dataspace>(vbus));
    l4_addr_t mmio_virt = 0;
    int r = L4Re::Env::env()->rm()->attach(
      &mmio_virt, mmio_size + page_offset,
      L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW
      | L4Re::Rm::F::Cache_uncached,
      iods, mmio_start - page_offset, L4_PAGESHIFT);
    if (r < 0) {
      printf("[netd] failed to map MMIO 0x%lx (err %d)\n", mmio_start, r);
      continue;
    }
    l4_addr_t dev_base = mmio_virt + page_offset;

    volatile l4_uint32_t *regs =
      reinterpret_cast<volatile l4_uint32_t *>(dev_base);
    if (regs[VTMMIO_MAGIC / 4] != 0x74726976u
        || regs[VTMMIO_DEVICE_ID / 4] != VTDEV_NET) {
      L4Re::Env::env()->rm()->detach(mmio_virt, nullptr);
      continue;
    }

    printf("[netd] virtio-net at vbus MMIO 0x%lx\n", mmio_start);
    *virt_out = dev_base;
    return true;
  }

  printf("[netd] no virtio-net device on vbus\n");
  return false;
}

/* ------------------------------------------------------------------ */
/* DMA allocation                                                       */
/* ------------------------------------------------------------------ */
static bool net_alloc_dma(struct virtnet_dma **dma_out, l4_uint64_t *phys_out)
{
  constexpr l4_size_t DMA_SIZE   = sizeof(struct virtnet_dma);
  constexpr l4_size_t ALLOC_SIZE =
    (DMA_SIZE + L4_PAGESIZE - 1) & ~(l4_size_t)(L4_PAGESIZE - 1);

  auto dma_space = L4Re::chkcap(
    L4Re::Util::make_unique_cap<L4Re::Dma_space>(), "alloc DMA space cap");
  L4Re::chksys(
    L4Re::Env::env()->user_factory()->create(dma_space.get()),
    "create DMA space");
  L4Re::chksys(
    dma_space->associate(L4::Ipc::Cap<L4::Task>(), L4Re::Dma_space::Phys_space),
    "associate DMA space (phys bypass)");

  auto ds = L4Re::chkcap(
    L4Re::Util::make_unique_cap<L4Re::Dataspace>(), "alloc DMA DS cap");
  L4Re::chksys(
    L4Re::Env::env()->mem_alloc()->alloc(
      ALLOC_SIZE, ds.get(),
      L4Re::Mem_alloc::Continuous | L4Re::Mem_alloc::Pinned),
    "alloc DMA memory");

  l4_addr_t vaddr = 0;
  L4Re::chksys(
    L4Re::Env::env()->rm()->attach(
      &vaddr, ALLOC_SIZE,
      L4Re::Rm::F::Search_addr | L4Re::Rm::F::Eager_map | L4Re::Rm::F::RW,
      L4::Ipc::make_cap_rw(ds.get())),
    "attach DMA memory");
  memset(reinterpret_cast<void *>(vaddr), 0, ALLOC_SIZE);

  l4_size_t mapped_size = ALLOC_SIZE;
  L4Re::Dma_space::Dma_addr dma_addr = 0;
  L4Re::chksys(
    dma_space->map(
      L4::Ipc::make_cap(ds.get(), L4_CAP_FPAGE_RW),
      0, &mapped_size,
      L4Re::Dma_space::Attributes::None,
      L4Re::Dma_space::Direction::Bidirectional,
      &dma_addr),
    "map DMA memory → phys addr");

  printf("[netd] DMA @ virt=%p phys=0x%llx\n",
         (void *)vaddr, (unsigned long long)dma_addr);

  (void)ds.release();
  (void)dma_space.release();

  *dma_out  = reinterpret_cast<struct virtnet_dma *>(vaddr);
  *phys_out = (l4_uint64_t)dma_addr;
  return true;
}

/* ------------------------------------------------------------------ */
/* DHCP client                                                          */
/* ------------------------------------------------------------------ */
static bool do_dhcp(int timeout_ms)
{
  printf("[netd] dhcp: requesting lease on vn0 ...\n");
  if (netifapi_dhcp_start(&n_netif) != ERR_OK) {
    printf("[netd] dhcp: dhcp_start failed\n");
    return false;
  }

  int waited = 0;
  while (waited < timeout_ms && !dhcp_supplied_address(&n_netif)) {
    sys_msleep(100);
    waited += 100;
  }
  if (!dhcp_supplied_address(&n_netif)) {
    printf("[netd] dhcp: no offer after %d ms\n", timeout_ms);
    return false;
  }

  printf("[netd] dhcp: bound IP %s", ip4addr_ntoa(netif_ip4_addr(&n_netif)));
  printf(" netmask %s", ip4addr_ntoa(netif_ip4_netmask(&n_netif)));
  printf(" gw %s\n", ip4addr_ntoa(netif_ip4_gw(&n_netif)));

  if (ip_addr_isany(dns_getserver(0))) {
    ip_addr_t dns;
    IP_ADDR4(&dns, 10, 0, 2, 3);
    dns_setserver(0, &dns);
  }
  printf("[netd] dhcp: DNS %s\n", ipaddr_ntoa(dns_getserver(0)));
  return true;
}

/* ------------------------------------------------------------------ */
/* IP configuration                                                     */
/* ------------------------------------------------------------------ */
static void net_configure_ip()
{
  const char *cfg = getenv("IFCONFIG_IP4_vn0");
  if (!cfg)
    cfg = "10.0.2.15/24 via 10.0.2.2";

  if (strcmp(cfg, "dhcp") == 0) {
    if (do_dhcp(8000))
      return;
    printf("[netd] DHCP failed, falling back to static 10.0.2.15/24\n");
    cfg = "10.0.2.15/24 via 10.0.2.2";
  }

  ip4_addr_t ip, nm, gw;
  unsigned prefix = 24;
  char ip_str[20] = {}, gw_str[20] = {};
  unsigned ip0, ip1, ip2, ip3, gw0, gw1, gw2, gw3;

  int n = sscanf(cfg, "%u.%u.%u.%u/%u via %u.%u.%u.%u",
                 &ip0, &ip1, &ip2, &ip3, &prefix,
                 &gw0, &gw1, &gw2, &gw3);

  snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip0, ip1, ip2, ip3);
  ip4addr_aton(ip_str, &ip);

  {
    unsigned p = (prefix > 32) ? 32 : prefix;
    unsigned m = p ? (~0u << (32 - p)) : 0u;
    char nm_str[20];
    snprintf(nm_str, sizeof(nm_str), "%u.%u.%u.%u",
             (m >> 24) & 0xff, (m >> 16) & 0xff,
             (m >> 8)  & 0xff,  m        & 0xff);
    ip4addr_aton(nm_str, &nm);
  }

  if (n >= 9) {
    snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", gw0, gw1, gw2, gw3);
    ip4addr_aton(gw_str, &gw);
  } else {
    ip4addr_aton("10.0.2.2", &gw);
  }

  LOCK_TCPIP_CORE();
  netif_set_addr(&n_netif, &ip, &nm, &gw);
  UNLOCK_TCPIP_CORE();

  {
    char ip_s[16], nm_s[16], gw_s[16];
    snprintf(ip_s, sizeof(ip_s), "%s", ip4addr_ntoa(&ip));
    snprintf(nm_s, sizeof(nm_s), "%s", ip4addr_ntoa(&nm));
    snprintf(gw_s, sizeof(gw_s), "%s", ip4addr_ntoa(&gw));
    printf("[netd] IP %s netmask %s gw %s\n", ip_s, nm_s, gw_s);
  }

  ip_addr_t dns_addr;
  IP_ADDR4(&dns_addr, 10, 0, 2, 3);
  dns_setserver(0, &dns_addr);
  printf("[netd] DNS server set to 10.0.2.3\n");
}

/* ------------------------------------------------------------------ */
/* Network stack bring-up                                               */
/* ------------------------------------------------------------------ */
static bool net_stack_init()
{
  struct virtnet_dma *vdma = nullptr;
  l4_uint64_t         phys = 0;
  if (!net_alloc_dma(&vdma, &phys)) {
    printf("[netd] DMA allocation failed\n");
    return false;
  }

  l4_addr_t mmio_virt = 0;
  if (!net_map_mmio(&mmio_virt)) {
    printf("[netd] no virtio-net device, network unavailable\n");
    return false;
  }

  n_rx_last_used = 0;
  n_tx_last_used = 0;
  n_tx_avail_idx = 0;
  if (!net_virtio_init(mmio_virt, phys, vdma)) {
    printf("[netd] virtio init failed\n");
    return false;
  }

  ip4_addr_t zero;
  ip4_addr_set_zero(&zero);
  if (netifapi_netif_add(&n_netif, &zero, &zero, &zero,
                         nullptr, net_netif_init, ethernet_input) != ERR_OK) {
    printf("[netd] netif_add failed\n");
    return false;
  }
  LOCK_TCPIP_CORE();
  netif_set_up(&n_netif);
  netif_set_default(&n_netif);
  UNLOCK_TCPIP_CORE();

  net_configure_ip();

  g_net_stack_ready = true;
  printf("[netd] stack ready, IP %s\n",
         ip4addr_ntoa(netif_ip4_addr(&n_netif)));
  return true;
}

/* ------------------------------------------------------------------ */
/* Net_svr socket server                                                */
/* ------------------------------------------------------------------ */
namespace {

/* Open TCP connection handles.  Phase 1 uses the lwIP fd directly as the
 * opaque handle; the set guards against clients passing fds we never opened. */
static std::set<int> g_open_handles;

/* ---- ICMP ping helpers (ported from native_shell's cmd_ping.cc) ---- */
#define PING_DATA_SIZE  56u
#define PING_PKT_SIZE   (sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE)
#define PING_ID         0xABCDu

static uint16_t icmp_chksum(const void *data, size_t len)
{
  const uint16_t *p = (const uint16_t *)data;
  uint32_t sum = 0;
  while (len > 1) { sum += *p++; len -= 2; }
  if (len) sum += *(const uint8_t *)p;
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

/* ---- Phase 2: echo-server worker threads ----
 * These run a whole server (bind/accept/echo) on their own detached pthread, so
 * the main server.loop stays responsive to other clients.  lwIP is thread-safe
 * (its tcpip thread + per-socket locking), so blocking socket calls here are
 * fine.  Started by the udp_echo / tcp_echo RPCs; run until netd exits. */
static volatile bool g_udp_echo_running = false;
static volatile bool g_tcp_echo_running = false;

static void *udp_echo_thread(void *arg)
{
  int port = (int)(long)arg;
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) { g_udp_echo_running = false; return nullptr; }
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons((uint16_t)port);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    { printf("[netd] udp echo: bind port %d failed\n", port);
      close(s); g_udp_echo_running = false; return nullptr; }

  printf("[netd] udp echo server listening on port %d\n", port);
  char buf[1024];
  for (;;)
    {
      struct sockaddr_in cli{};
      socklen_t clen = sizeof(cli);
      ssize_t n = recvfrom(s, buf, sizeof(buf), 0,
                           (struct sockaddr *)&cli, &clen);
      if (n < 0) break;
      sendto(s, buf, (size_t)n, 0, (struct sockaddr *)&cli, clen);
    }
  close(s);
  g_udp_echo_running = false;
  return nullptr;
}

static void *tcp_echo_thread(void *arg)
{
  int port = (int)(long)arg;
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { g_tcp_echo_running = false; return nullptr; }
  int on = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons((uint16_t)port);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0
      || listen(srv, 4) < 0)
    { printf("[netd] tcp echo: bind/listen port %d failed\n", port);
      close(srv); g_tcp_echo_running = false; return nullptr; }

  printf("[netd] tcp echo server listening on port %d\n", port);
  for (;;)
    {
      struct sockaddr_in cli{};
      socklen_t clen = sizeof(cli);
      int cfd = accept(srv, (struct sockaddr *)&cli, &clen);
      if (cfd < 0) continue;
      const char *banner = "Hello from TuringOS netd TCP echo server!\r\n";
      send(cfd, banner, strlen(banner), 0);
      char buf[512];
      ssize_t n;
      while ((n = recv(cfd, buf, sizeof(buf), 0)) > 0)
        send(cfd, buf, (size_t)n, 0);
      close(cfd);
    }
  close(srv);
  g_tcp_echo_running = false;
  return nullptr;
}

/* Spawn a detached worker thread. */
static bool spawn_worker(void *(*fn)(void *), void *arg)
{
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&t, &a, fn, arg);
  pthread_attr_destroy(&a);
  return r == 0;
}

class Net_impl : public L4::Epiface_t<Net_impl, Net_svr>
{
public:
  long op_tcp_connect(Net_svr::Rights, l4_uint32_t ip_be, l4_uint16_t port,
                      l4_uint32_t &handle)
  {
    if (!g_net_stack_ready)
      return -L4_ENODEV;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return -L4_ENOMEM;

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = ip_be;          /* already network byte order */

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr),
                sizeof(addr)) < 0) {
      close(fd);
      return -L4_EIO;
    }

    /* Bound recv() so a client that waits for data (e.g. mqtt sub) lets netd's
     * server.loop poll instead of blocking it indefinitely.  op_recv maps the
     * timeout to an empty result; the client loops. */
    struct timeval rtv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    g_open_handles.insert(fd);
    handle = (l4_uint32_t)fd;
    printf("[netd] tcp_connect -> handle %u (%s:%u)\n",
           handle, inet_ntoa(addr.sin_addr), port);
    return L4_EOK;
  }

  long op_send(Net_svr::Rights, l4_uint32_t handle,
               L4::Ipc::Array_ref<char const> data, l4_uint32_t &sent)
  {
    int fd = (int)handle;
    if (g_open_handles.find(fd) == g_open_handles.end())
      return -L4_EINVAL;

    ssize_t n = ::send(fd, data.data, data.length, 0);
    if (n < 0)
      return -L4_EIO;
    sent = (l4_uint32_t)n;
    return L4_EOK;
  }

  long op_recv(Net_svr::Rights, l4_uint32_t handle, l4_uint32_t max,
               L4::Ipc::Array_ref<char> &data)
  {
    int fd = (int)handle;
    if (g_open_handles.find(fd) == g_open_handles.end())
      return -L4_EINVAL;

    if (max > sizeof(_rxbuf))
      max = sizeof(_rxbuf);

    ssize_t n = ::recv(fd, _rxbuf, max, 0);   /* bounded by SO_RCVTIMEO */
    if (n < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          n = 0;                 /* timeout: no data this poll (length 0) */
        else
          return -L4_EIO;
      }

    /* length 0 = no data (recv timeout) or peer EOF; the client decides. */
    data = L4::Ipc::Array_ref<char>((unsigned short)n, _rxbuf);
    return L4_EOK;
  }

  long op_close(Net_svr::Rights, l4_uint32_t handle)
  {
    int fd = (int)handle;
    auto it = g_open_handles.find(fd);
    if (it == g_open_handles.end())
      return -L4_EINVAL;
    close(fd);
    g_open_handles.erase(it);
    return L4_EOK;
  }

  long op_ifconfig(Net_svr::Rights, L4::Ipc::Array_ref<char> &text)
  {
    int o = 0;
    auto app = [&](const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
      if (o >= (int)sizeof(_txtbuf)) return;
      va_list ap; va_start(ap, fmt);
      int w = vsnprintf(_txtbuf + o, sizeof(_txtbuf) - o, fmt, ap);
      va_end(ap);
      if (w > 0) o += w;
    };

    LOCK_TCPIP_CORE();
    if (!g_net_stack_ready || netif_list == nullptr)
      {
        UNLOCK_TCPIP_CORE();
        text = L4::Ipc::Array_ref<char>(0, _txtbuf);
        return L4_EOK;
      }

    for (struct netif *nif = netif_list; nif; nif = nif->next)
      {
        char flags[64] = {}; char *fp = flags; unsigned fval = 0;
        if (nif->flags & NETIF_FLAG_UP)        { fval |= 0x0001; fp += snprintf(fp, 16, "UP,"); }
        if (nif->flags & NETIF_FLAG_BROADCAST) { fval |= 0x0002; fp += snprintf(fp, 16, "BROADCAST,"); }
        if (nif->flags & NETIF_FLAG_LINK_UP)   { fval |= 0x0040; fp += snprintf(fp, 16, "RUNNING,"); }
        if (nif->flags & NETIF_FLAG_ETHARP)    { fval |= 0x1000; fp += snprintf(fp, 16, "MULTICAST,"); }
        if (fp > flags) *(fp - 1) = '\0';

        app("%c%c%u: flags=%04x<%s>  mtu %u\n",
            nif->name[0], nif->name[1], (unsigned)nif->num, fval, flags,
            (unsigned)nif->mtu);

        const ip4_addr_t *ip = netif_ip4_addr(nif);
        const ip4_addr_t *nm = netif_ip4_netmask(nif);
        const ip4_addr_t *gw = netif_ip4_gw(nif);
        if (!ip4_addr_isany(ip))
          {
            ip4_addr_t bc; bc.addr = ip->addr | ~nm->addr;
            char ips[16], nms[16], bcs[16], gws[16];
            snprintf(ips, sizeof(ips), "%s", ip4addr_ntoa(ip));
            snprintf(nms, sizeof(nms), "%s", ip4addr_ntoa(nm));
            snprintf(bcs, sizeof(bcs), "%s", ip4addr_ntoa(&bc));
            snprintf(gws, sizeof(gws), "%s", ip4addr_ntoa(gw));
            app("        inet %s  netmask %s  broadcast %s\n", ips, nms, bcs);
            app("        gateway %s\n", gws);
          }
        if (nif->hwaddr_len == 6)
          app("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
              nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
              nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5]);
      }
    UNLOCK_TCPIP_CORE();

    if (o > (int)sizeof(_txtbuf)) o = sizeof(_txtbuf);
    text = L4::Ipc::Array_ref<char>((unsigned short)o, _txtbuf);
    return L4_EOK;
  }

  long op_resolve(Net_svr::Rights, L4::Ipc::Array_ref<char const> host,
                  l4_uint32_t &ip_be)
  {
    if (!g_net_stack_ready)
      return -L4_ENODEV;

    char name[128];
    unsigned n = host.length < sizeof(name) - 1 ? host.length : sizeof(name) - 1;
    memcpy(name, host.data, n);
    name[n] = '\0';

    /* dotted-decimal short-circuit */
    ip4_addr_t a;
    if (ip4addr_aton(name, &a)) { ip_be = a.addr; return L4_EOK; }

    struct addrinfo hints{};
    struct addrinfo *res = nullptr;
    hints.ai_family = AF_INET;
    if (lwip_getaddrinfo(name, nullptr, &hints, &res) != 0 || !res)
      {
        if (res) lwip_freeaddrinfo(res);
        return -L4_ENOENT;
      }
    ip_be = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    lwip_freeaddrinfo(res);
    return L4_EOK;
  }

  long op_ping_one(Net_svr::Rights, l4_uint32_t ip_be, l4_uint32_t seq,
                   l4_int32_t &rtt_us, L4::Ipc::Array_ref<char> &line)
  {
    rtt_us = -1;
    auto ret = [&](int n) -> long
    {
      line = L4::Ipc::Array_ref<char>((unsigned short)n, _txtbuf);
      return L4_EOK;
    };

    if (!g_net_stack_ready)
      return ret(snprintf(_txtbuf, sizeof(_txtbuf), "ping: network not ready"));

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0)
      return ret(snprintf(_txtbuf, sizeof(_txtbuf),
                          "ping: cannot open raw socket"));
    struct timeval tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[PING_PKT_SIZE];
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
    dst.sin_addr.s_addr = ip_be;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (sendto(s, pkt, PING_PKT_SIZE, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0)
      { close(s); return ret(snprintf(_txtbuf, sizeof(_txtbuf),
                                      "ping: send error")); }

    uint8_t buf[256];
    struct sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    ssize_t rn = recvfrom(s, buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromlen);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    close(s);

    if (rn < 0)
      return ret(snprintf(_txtbuf, sizeof(_txtbuf),
                          "Request timeout for icmp_seq %u", seq));
    if (rn < (ssize_t)sizeof(struct ip_hdr))
      return ret(snprintf(_txtbuf, sizeof(_txtbuf), "ping: short reply"));

    auto *ip = (struct ip_hdr *)buf;
    int iphlen = IPH_HL(ip) * 4;
    auto *reply = (struct icmp_echo_hdr *)(buf + iphlen);
    if (rn < iphlen + (int)sizeof(struct icmp_echo_hdr)
        || ICMPH_TYPE(reply) != ICMP_ER || ntohs(reply->id) != PING_ID)
      return ret(snprintf(_txtbuf, sizeof(_txtbuf),
                          "Request timeout for icmp_seq %u", seq));

    long us = (t1.tv_sec - t0.tv_sec) * 1000000L
            + (t1.tv_nsec - t0.tv_nsec) / 1000L;
    rtt_us = (l4_int32_t)us;
    return ret(snprintf(_txtbuf, sizeof(_txtbuf),
                        "%zd bytes from %s: icmp_seq=%u ttl=%d time=%ld.%ld ms",
                        rn - iphlen, inet_ntoa(from.sin_addr),
                        ntohs(reply->seqno), (int)IPH_TTL(ip),
                        us / 1000, (us % 1000) / 100));
  }

  long op_dhcp(Net_svr::Rights, l4_uint32_t action,
               L4::Ipc::Array_ref<char> &text)
  {
    int o = 0;
    auto app = [&](const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
      if (o >= (int)sizeof(_txtbuf)) return;
      va_list ap; va_start(ap, fmt);
      int w = vsnprintf(_txtbuf + o, sizeof(_txtbuf) - o, fmt, ap);
      va_end(ap);
      if (w > 0) o += w;
    };

    if (!g_net_stack_ready)
      {
        app("dhcp: network not ready\n");
        text = L4::Ipc::Array_ref<char>((unsigned short)o, _txtbuf);
        return L4_EOK;
      }

    if (action == 1)            /* release */
      {
        netifapi_dhcp_release_and_stop(&n_netif);
        app("dhcp: lease released, DHCP stopped\n");
      }
    else                        /* 0 = renew, 2 = status */
      {
        if (action == 0)
          {
            if (!do_dhcp(8000))
              {
                app("dhcp: no lease (no offer)\n");
                text = L4::Ipc::Array_ref<char>((unsigned short)o, _txtbuf);
                return L4_EOK;
              }
          }
        char ip[16], nm[16], gw[16];
        LOCK_TCPIP_CORE();
        snprintf(ip, sizeof(ip), "%s", ip4addr_ntoa(netif_ip4_addr(&n_netif)));
        snprintf(nm, sizeof(nm), "%s", ip4addr_ntoa(netif_ip4_netmask(&n_netif)));
        snprintf(gw, sizeof(gw), "%s", ip4addr_ntoa(netif_ip4_gw(&n_netif)));
        UNLOCK_TCPIP_CORE();
        app("dhcp: IP %s  netmask %s  gw %s\n", ip, nm, gw);
        app("dhcp: DNS %s\n", ipaddr_ntoa(dns_getserver(0)));
      }

    text = L4::Ipc::Array_ref<char>((unsigned short)o, _txtbuf);
    return L4_EOK;
  }

  long op_udp_echo(Net_svr::Rights, l4_uint32_t port,
                   L4::Ipc::Array_ref<char> &text)
  {
    int n;
    if (!g_net_stack_ready)
      n = snprintf(_txtbuf, sizeof(_txtbuf), "udp: network not ready");
    else if (port == 0 || port > 65535)
      n = snprintf(_txtbuf, sizeof(_txtbuf), "udp: bad port");
    else if (g_udp_echo_running)
      n = snprintf(_txtbuf, sizeof(_txtbuf),
                   "udp: echo server already running");
    else
      {
        g_udp_echo_running = true;
        if (!spawn_worker(udp_echo_thread, (void *)(long)port))
          { g_udp_echo_running = false;
            n = snprintf(_txtbuf, sizeof(_txtbuf), "udp: cannot start server"); }
        else
          n = snprintf(_txtbuf, sizeof(_txtbuf),
                       "udp: echo server started on port %u", port);
      }
    text = L4::Ipc::Array_ref<char>((unsigned short)n, _txtbuf);
    return L4_EOK;
  }

  long op_tcp_echo(Net_svr::Rights, l4_uint32_t port,
                   L4::Ipc::Array_ref<char> &text)
  {
    int n;
    if (!g_net_stack_ready)
      n = snprintf(_txtbuf, sizeof(_txtbuf), "net: network not ready");
    else if (port == 0 || port > 65535)
      n = snprintf(_txtbuf, sizeof(_txtbuf), "net: bad port");
    else if (g_tcp_echo_running)
      n = snprintf(_txtbuf, sizeof(_txtbuf),
                   "net: echo server already running");
    else
      {
        g_tcp_echo_running = true;
        if (!spawn_worker(tcp_echo_thread, (void *)(long)port))
          { g_tcp_echo_running = false;
            n = snprintf(_txtbuf, sizeof(_txtbuf), "net: cannot start server"); }
        else
          n = snprintf(_txtbuf, sizeof(_txtbuf),
                       "net: TCP echo server started on port %u", port);
      }
    text = L4::Ipc::Array_ref<char>((unsigned short)n, _txtbuf);
    return L4_EOK;
  }

private:
  /* Recv staging buffer.  Sized for the inline IPC message-register budget
   * (UTCB), which caps a single reply at well under 2 KiB. */
  char _rxbuf[1600];
  /* Text buffer for the formatted-output ops (ifconfig / ping / dhcp).  Single
   * client, so these never overlap. */
  char _txtbuf[2000];
};

} // namespace

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;

int main()
{
  printf("[netd] starting\n");

  /* Bring up the NIC + lwIP before publishing the service, so the first
   * client call already finds a usable stack.  (lwip's l4libinit constructor
   * has already started the tcpip thread / lwip_init.) */
  if (!net_stack_init())
    printf("[netd] WARNING: network stack init failed — RPCs will return "
           "-ENODEV\n");

  static Net_impl impl;

  /* Bind the ned-provided service gate ("svr") so native_shell's pre-wired
   * netd channel reaches us. */
  if (!server.registry()->register_obj(&impl, "svr").is_valid()) {
    printf("[netd] ERROR: cannot bind 'svr' gate — exiting\n");
    return 1;
  }
  printf("[netd] service ready (Net_svr on 'svr')\n");

  server.loop();
  return 0;
}
