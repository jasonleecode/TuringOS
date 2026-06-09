/*
 * cmd_net.cc — "net" command for native_shell
 *
 * Starts a virtio-net driver + lwIP TCP echo server in a background thread.
 * The shell remains interactive while the server runs.
 *
 * Usage:
 *   net          – start TCP echo server on port 5000
 *   net status   – show whether the server is running
 *   net stop     – (not yet implemented, kills the accept loop)
 *
 * Boot prerequisites (ned config + QEMU flags):
 *   caps:  vbus (virt-net.io), sigma0
 *   QEMU:  -netdev user,id=net0,hostfwd=tcp::5555-:5000
 *          -device virtio-net-device,netdev=net0
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

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
#include <netif/ethernet.h>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/dma_space>
#include <l4/re/mem_alloc>
#include <l4/util/util.h>
#include <l4/re/rm>
#include <l4/re/util/unique_cap>
#include <l4/sys/consts.h>
#include <l4/vbus/vbus>
#include <l4/sigma0/sigma0.h>

#include <pthread.h>
#include <pthread-l4.h>

#include "commands.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */
#ifndef NET_TCP_PORT
#  define NET_TCP_PORT  5000
#endif

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
 * VIRTIO_NET_F_MRG_RXBUF: 10 bytes, no num_buffers field.
 * Our struct vnet_hdr is 12 bytes (includes num_buffers), but QEMU
 * only writes/reads 10 bytes when MRG_RXBUF is not negotiated. */
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

static bool g_net_running = false;     /* true once server thread started */
       bool g_net_stack_ready = false; /* true once hw+lwIP init done */

bool net_is_ready() { return g_net_stack_ready; }

static pthread_mutex_t g_net_init_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_net_init_cv   = PTHREAD_COND_INITIALIZER;
static bool            g_net_init_done = false;

static void net_signal_init()
{
  pthread_mutex_lock(&g_net_init_mtx);
  g_net_init_done = true;
  pthread_cond_signal(&g_net_init_cv);
  pthread_mutex_unlock(&g_net_init_mtx);
}

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

  klog_info(KLOG_NET, "net: MAC %02x:%02x:%02x:%02x:%02x:%02x",
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
    klog_err(KLOG_NET, "net: failed to create RX thread");
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
  klog_info(KLOG_NET, "net: legacy protocol (v1)");
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
     while the device is active. A QUEUE_NOTIFY sent before DRIVER_OK is
     silently discarded by QEMU's virtio-mmio implementation. */
  nreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_DRIVER_OK);
  __sync_synchronize();

  net_prefill_rx(qsz);   /* fills avail ring and sends QUEUE_NOTIFY(0) */

  klog_info(KLOG_NET, "net: device ready (v1 legacy)");
  return true;
}

static bool net_virtio_init_v2()
{
  klog_info(KLOG_NET, "net: modern protocol (v2)");
  nreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER);

  nreg_w(VTMMIO_DEV_FEAT_SEL, 0);
  l4_uint32_t feat0 = nreg_r(VTMMIO_DEV_FEAT) & VTNET_F_MAC;
  nreg_w(VTMMIO_DEV_FEAT_SEL, 1);
  l4_uint32_t feat1 = nreg_r(VTMMIO_DEV_FEAT) & VT_F_VERSION_1;
  if (!(feat1 & VT_F_VERSION_1)) {
    klog_err(KLOG_NET, "net: device does not support VERSION_1");
    return false;
  }
  nreg_w(VTMMIO_DRV_FEAT_SEL, 0); nreg_w(VTMMIO_DRV_FEAT, feat0);
  nreg_w(VTMMIO_DRV_FEAT_SEL, 1); nreg_w(VTMMIO_DRV_FEAT, feat1);

  nreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_FEATURES_OK);
  __sync_synchronize();
  if (!(nreg_r(VTMMIO_STATUS) & VTSTS_FEATURES_OK)) {
    klog_err(KLOG_NET, "net: FEATURES_OK rejected");
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
  klog_info(KLOG_NET, "net: device ready (v2 modern)");
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
    klog_err(KLOG_NET, "net: bad magic 0x%08x", magic);
    return false;
  }
  if (devid != VTDEV_NET) {
    klog_err(KLOG_NET, "net: device_id=%u, expected %u", devid, VTDEV_NET);
    return false;
  }
  if (version != 1 && version != 2) {
    klog_err(KLOG_NET, "net: unsupported version %u", version);
    return false;
  }

  klog_info(KLOG_NET, "net: found virtio-net device (v%u)", version);
  nreg_w(VTMMIO_STATUS, 0);
  __sync_synchronize();

  return (version == 1) ? net_virtio_init_v1() : net_virtio_init_v2();
}

/* ------------------------------------------------------------------ */
/* Sigma0 MMIO scan                                                     */
/* ------------------------------------------------------------------ */
static bool net_map_mmio(l4_addr_t *virt_out)
{
  constexpr l4_addr_t VIRTIO_BASE = 0x0a000000UL;
  constexpr unsigned  NUM_PAGES   = 4;
  constexpr l4_size_t MAP_SIZE    = NUM_PAGES * L4_PAGESIZE;

  auto sigma0 = L4Re::Env::env()->get_cap<void>("sigma0");
  if (!sigma0.is_valid()) {
    klog_err(KLOG_NET, "net: sigma0 cap not found");
    return false;
  }

  l4_addr_t vbase = 0;
  if (L4Re::Env::env()->rm()->reserve_area(
        &vbase, MAP_SIZE, L4Re::Rm::F::Search_addr, L4_PAGESHIFT) < 0) {
    klog_err(KLOG_NET, "net: reserve MMIO area failed");
    return false;
  }

  for (unsigned p = 0; p < NUM_PAGES; ++p) {
    l4_addr_t phys = VIRTIO_BASE + (l4_addr_t)p * L4_PAGESIZE;
    l4_addr_t virt = vbase       + (l4_addr_t)p * L4_PAGESIZE;
    if (l4sigma0_map_iomem(sigma0.cap(), phys, virt, L4_PAGESIZE, 0) < 0) {
      klog_err(KLOG_NET, "net: sigma0 iomem map failed page %u", p);
      L4Re::Env::env()->rm()->free_area(vbase);
      return false;
    }
  }

  for (unsigned i = 0; i < 32; ++i) {
    volatile l4_uint32_t *regs =
      reinterpret_cast<volatile l4_uint32_t *>(vbase + i * 0x200u);
    if (regs[0] != 0x74726976u)
      continue;
    l4_uint32_t devid = regs[0x008 / 4];
    if (devid == VTDEV_NET) {
      klog_info(KLOG_NET, "net: virtio-net found at slot %u (phys 0x%08lx)",
                i, VIRTIO_BASE + i * 0x200u);
      *virt_out = (l4_addr_t)regs;
      return true;
    }
  }

  klog_warn(KLOG_NET, "net: no virtio-net in MMIO scan");
  L4Re::Env::env()->rm()->free_area(vbase);
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

  klog_info(KLOG_NET, "net: DMA @ virt=%p phys=0x%llx",
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
/* Acquire an IPv4 lease via DHCP on vn0.  lwIP's tcpip thread drives the
 * DHCP state machine + timers; we kick it off (thread-safe netifapi) and
 * poll dhcp_supplied_address() until bound or timeout.  Returns true on bind.
 * Works whether the netif currently has a static address or none — dhcp_start
 * issues a fresh DISCOVER and takes over the interface. */
static bool do_dhcp(int timeout_ms)
{
  printf("dhcp: requesting lease on vn0 ...\n");
  if (netifapi_dhcp_start(&n_netif) != ERR_OK) {
    printf("dhcp: dhcp_start failed\n");
    return false;
  }

  int waited = 0;
  while (waited < timeout_ms && !dhcp_supplied_address(&n_netif)) {
    sys_msleep(100);
    waited += 100;
  }
  if (!dhcp_supplied_address(&n_netif)) {
    printf("dhcp: no offer after %d ms\n", timeout_ms);
    return false;
  }

  printf("dhcp: bound  IP %s", ip4addr_ntoa(netif_ip4_addr(&n_netif)));
  printf("  netmask %s", ip4addr_ntoa(netif_ip4_netmask(&n_netif)));
  printf("  gw %s\n", ip4addr_ntoa(netif_ip4_gw(&n_netif)));

  /* DHCP option 6 may supply DNS; otherwise fall back to the SLIRP resolver. */
  if (ip_addr_isany(dns_getserver(0))) {
    ip_addr_t dns;
    IP_ADDR4(&dns, 10, 0, 2, 3);
    dns_setserver(0, &dns);
  }
  printf("dhcp: DNS %s\n", ipaddr_ntoa(dns_getserver(0)));
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

  /* IFCONFIG_IP4_vn0=dhcp -> obtain the address dynamically.  On failure fall
   * back to the static default so the system still has connectivity. */
  if (strcmp(cfg, "dhcp") == 0) {
    if (do_dhcp(8000))
      return;
    printf("net: DHCP failed, falling back to static 10.0.2.15/24\n");
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
    /* Build netmask from prefix length as dotted-decimal string, then
       use ip4addr_aton() which handles byte-order correctly at runtime. */
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

  printf("net: IP %s", ip4addr_ntoa(&ip));
  printf("  netmask %s", ip4addr_ntoa(&nm));
  printf("  gw %s\n", ip4addr_ntoa(&gw));

  /* Configure QEMU slirp DNS server (always at 10.0.2.3 in user-mode NAT) */
  ip_addr_t dns_addr;
  IP_ADDR4(&dns_addr, 10, 0, 2, 3);
  dns_setserver(0, &dns_addr);
  klog_info(KLOG_NET, "net: DNS server set to 10.0.2.3");
}

/* ------------------------------------------------------------------ */
/* TCP echo server (runs in background thread)                          */
/* ------------------------------------------------------------------ */
static void handle_client(int cfd, struct sockaddr_in *cli)
{
  printf("net: client connected from %s:%d\n",
         inet_ntoa(cli->sin_addr), ntohs(cli->sin_port));

  const char *banner = "Hello from TuringOS native shell TCP server!\r\n";
  send(cfd, banner, strlen(banner), 0);

  char buf[512];
  ssize_t n;
  while ((n = recv(cfd, buf, sizeof(buf) - 1, 0)) > 0) {
    buf[n] = '\0';
    printf("net: rx %zd bytes: %s", n, buf);
    send(cfd, buf, (size_t)n, 0);
  }

  printf("net: client disconnected\n");
  close(cfd);
}

static void *net_stack_init_thread(void * /*arg*/)
try {
  /* ---- Allocate DMA buffer ---- */
  struct virtnet_dma *vdma = nullptr;
  l4_uint64_t         phys = 0;
  if (!net_alloc_dma(&vdma, &phys)) {
    klog_err(KLOG_NET, "net: DMA allocation failed");
    return nullptr;
  }

  /* ---- Map virtio MMIO ---- */
  l4_addr_t mmio_virt = 0;
  if (!net_map_mmio(&mmio_virt)) {
    klog_warn(KLOG_NET, "net: no virtio-net device, skipping network init");
    return nullptr;
  }

  /* ---- Initialise virtio hardware ---- */
  n_rx_last_used = 0;
  n_tx_last_used = 0;
  n_tx_avail_idx = 0;
  if (!net_virtio_init(mmio_virt, phys, vdma)) {
    klog_err(KLOG_NET, "net: virtio init failed");
    return nullptr;
  }

  /* ---- Register lwIP netif ---- */
  ip4_addr_t zero;
  ip4_addr_set_zero(&zero);
  if (netifapi_netif_add(&n_netif, &zero, &zero, &zero,
                         nullptr, net_netif_init, ethernet_input) != ERR_OK) {
    klog_err(KLOG_NET, "net: netif_add failed");
    return nullptr;
  }
  LOCK_TCPIP_CORE();
  netif_set_up(&n_netif);
  netif_set_default(&n_netif);
  UNLOCK_TCPIP_CORE();

  /* ---- Configure IP ---- */
  net_configure_ip();

  g_net_stack_ready = true;
  klog_info(KLOG_NET, "net: stack ready, IP %s",
            ip4addr_ntoa(netif_ip4_addr(&n_netif)));
  return nullptr;
} catch (...) {
  klog_err(KLOG_NET, "net: init exception, network unavailable");
  return nullptr;
}

static void *net_server_thread(void * /*arg*/)
{
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("net: socket");
    g_net_running = false;
    net_signal_init();
    return nullptr;
  }

  int on = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(NET_TCP_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("net: bind");
    close(srv);
    g_net_running = false;
    net_signal_init();
    return nullptr;
  }
  if (listen(srv, 4) < 0) {
    perror("net: listen");
    close(srv);
    g_net_running = false;
    net_signal_init();
    return nullptr;
  }

  printf("net: TCP echo server listening on port %d\n", NET_TCP_PORT);
  printf("net: test with: python3 tools/tcp_client.py\n");

  net_signal_init();
  task_register("net", "TCP echo server on port 5000");

  for (;;) {
    struct sockaddr_in cli{};
    socklen_t cli_len = sizeof(cli);
    int cfd = accept(srv, reinterpret_cast<struct sockaddr *>(&cli), &cli_len);
    if (cfd < 0) {
      perror("net: accept");
      continue;
    }
    handle_client(cfd, &cli);
  }

  task_unregister("net");
  g_net_running = false;
  return nullptr;
}

/* ------------------------------------------------------------------ */
/* Shell command entry point                                            */
/* ------------------------------------------------------------------ */
void cmd_net(int argc, char **argv)
{
  if (argc >= 2 && strcmp(argv[1], "status") == 0) {
    printf("net: stack %s, TCP server %s\n",
           g_net_stack_ready ? "ready" : "initializing",
           g_net_running     ? "running" : "not running");
    return;
  }

  if (!g_net_stack_ready) {
    printf("net: network stack not ready yet (auto-init in progress)\n");
    return;
  }

  if (g_net_running) {
    printf("net: TCP server already running\n");
    return;
  }

  g_net_running   = true;
  g_net_init_done = false;
  printf("net: starting TCP echo server...\n");

  pthread_t t;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &attr, net_server_thread, nullptr) != 0) {
    perror("net: pthread_create");
    g_net_running = false;
    pthread_attr_destroy(&attr);
    return;
  }
  pthread_attr_destroy(&attr);

  pthread_mutex_lock(&g_net_init_mtx);
  while (!g_net_init_done)
    pthread_cond_wait(&g_net_init_cv, &g_net_init_mtx);
  pthread_mutex_unlock(&g_net_init_mtx);
}

/* Called once at startup to initialise the network stack in background */
void net_auto_init()
{
  auto sigma0 = L4Re::Env::env()->get_cap<void>("sigma0");
  if (!sigma0.is_valid())
    return; /* no virtio-net available, skip silently */

  pthread_t t;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &attr, net_stack_init_thread, nullptr);
  pthread_attr_destroy(&attr);
}

/* ------------------------------------------------------------------ */
/* dhcp command                                                         */
/* ------------------------------------------------------------------ */
void cmd_dhcp(int argc, char **argv)
{
  if (!net_is_ready()) {
    printf("dhcp: network not ready\n");
    return;
  }
  if (argc > 1 && strcmp(argv[1], "release") == 0) {
    netifapi_dhcp_release_and_stop(&n_netif);
    printf("dhcp: lease released, DHCP stopped\n");
    return;
  }
  do_dhcp(8000);
}

/* ------------------------------------------------------------------ */
/* ifconfig command                                                     */
/* ------------------------------------------------------------------ */
void cmd_ifconfig(int argc, char **argv)
{
  (void)argc; (void)argv;

  LOCK_TCPIP_CORE();
  struct netif *nif = netif_list;
  if (!nif) {
    UNLOCK_TCPIP_CORE();
    printf("No network interfaces. Run 'net' first.\n");
    return;
  }

  for (; nif != nullptr; nif = nif->next) {
    char ifname[12];
    snprintf(ifname, sizeof(ifname), "%c%c%u",
             nif->name[0], nif->name[1], (unsigned)nif->num);

    /* Build flags string */
    char flags_str[64] = {};
    char *fp = flags_str;
    unsigned fval = 0;
    if (nif->flags & NETIF_FLAG_UP)        { fval |= 0x0001; fp += snprintf(fp, 16, "UP,");        }
    if (nif->flags & NETIF_FLAG_BROADCAST) { fval |= 0x0002; fp += snprintf(fp, 16, "BROADCAST,"); }
    if (nif->flags & NETIF_FLAG_LINK_UP)   { fval |= 0x0040; fp += snprintf(fp, 16, "RUNNING,");   }
    if (nif->flags & NETIF_FLAG_ETHARP)    { fval |= 0x1000; fp += snprintf(fp, 16, "MULTICAST,"); }
    if (fp > flags_str) *(fp - 1) = '\0'; /* remove trailing comma */

    printf("%s: flags=%04x<%s>  mtu %u\n",
           ifname, fval, flags_str, (unsigned)nif->mtu);

    /* IPv4 */
    const ip4_addr_t *ip   = netif_ip4_addr(nif);
    const ip4_addr_t *mask = netif_ip4_netmask(nif);
    const ip4_addr_t *gw   = netif_ip4_gw(nif);

    if (!ip4_addr_isany(ip)) {
      ip4_addr_t bcast;
      bcast.addr = ip->addr | ~mask->addr;

      char ip_s[16], mask_s[16], bcast_s[16], gw_s[16];
      snprintf(ip_s,    sizeof(ip_s),    "%s", ip4addr_ntoa(ip));
      snprintf(mask_s,  sizeof(mask_s),  "%s", ip4addr_ntoa(mask));
      snprintf(bcast_s, sizeof(bcast_s), "%s", ip4addr_ntoa(&bcast));
      snprintf(gw_s,    sizeof(gw_s),    "%s", ip4addr_ntoa(gw));

      printf("        inet %s  netmask %s  broadcast %s\n",
             ip_s, mask_s, bcast_s);
      printf("        gateway %s\n", gw_s);
    }

    /* MAC */
    if (nif->hwaddr_len == 6) {
      printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
             nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
             nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5]);
    }
    printf("\n");
  }
  UNLOCK_TCPIP_CORE();
}
