/*
 * TCP echo server – virtio-mmio netif driver + lwIP socket API.
 *
 * Hardware target: QEMU ARM virt (Cortex-A15, -M virt)
 *   virtio-net MMIO slot 0 → address 0xa000000, SPI 16 (GIC id 48)
 *
 * Network configuration via environment variable:
 *   IFCONFIG_IP4_vn0=10.0.2.15/24 via 10.0.2.2
 *   (or use the lwip_ifconfig_env library which reads the same vars)
 *
 * Boot caps needed in ned config:
 *   vbus   – vbus that exposes the virtio-net MMIO device
 *   sigma0 – to map uncached MMIO memory
 *
 * The lwip_virtio_net library (linked via REQUIRES_LIBS) auto-discovers
 * any cap named "virtnet*" in the initial caps list and registers it as
 * a lwIP netif. We therefore pass the virtnet cap from a p2p server OR
 * from a direct vbus binding.
 *
 * For direct QEMU virtio-net MMIO (no separate driver server), this file
 * implements the driver inline. Set CONFIG_USE_VBUS_VIRTIO=1 to use the
 * io-vbus path; otherwise the lwip_virtio_net / l4vio_net_p2p path is used.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

/* POSIX socket API – provided by libc_be_socket_lwip */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* ---- lwIP headers ---- */
#include <lwip/tcpip.h>
#include <lwip/netifapi.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>

/* ---- L4Re ---- */
#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/dma_space>
#include <l4/re/mem_alloc>
#include <l4/util/util.h>
#include <l4/re/rm>
#include <l4/re/util/unique_cap>
#include <l4/sys/consts.h>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_generic>
#include <l4/sigma0/sigma0.h>

#include <pthread.h>
#include <pthread-l4.h>

/* ------------------------------------------------------------------ */
/* Kconfig defaults                                                     */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_TCP_PORT
#  define CONFIG_TCP_PORT  5000
#endif

#ifndef CONFIG_VIRTIO_NET_MMIO_BASE
#  define CONFIG_VIRTIO_NET_MMIO_BASE  0xa000000UL
#endif
#ifndef CONFIG_VIRTIO_NET_MMIO_SIZE
#  define CONFIG_VIRTIO_NET_MMIO_SIZE  0x200UL
#endif

/* ------------------------------------------------------------------ */
/* Virtio-MMIO register offsets (virtio spec §4.2, version 2)          */
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
/* Modern (version 2) queue address registers */
#define VTMMIO_QUEUE_DESC_LO  0x080
#define VTMMIO_QUEUE_DESC_HI  0x084
#define VTMMIO_QUEUE_DRV_LO   0x090  /* available ring */
#define VTMMIO_QUEUE_DRV_HI   0x094
#define VTMMIO_QUEUE_DEV_LO   0x0a0  /* used ring */
#define VTMMIO_QUEUE_DEV_HI   0x0a4
#define VTMMIO_CONFIG_GEN     0x0fc
#define VTMMIO_CONFIG         0x100  /* device-specific config */

/* Legacy (version 1) extra registers */
#define VTMMIO_GUEST_PAGE_SIZE 0x028  /* WO: page size used by driver */
#define VTMMIO_QUEUE_ALIGN     0x03c  /* WO: used ring alignment      */
#define VTMMIO_QUEUE_PFN       0x040  /* RW: queue page frame number  */

/* Alignment used for the used ring within a legacy queue page.
 * used_off = align(desc_table + avail_ring, QUEUE_ALIGN_V1)
 * With VQ_SIZE=16: (256+38+63)&~63 = 320                          */
#define QUEUE_ALIGN_V1  64U
#define Q_USED_OFF      320U

/* On-wire virtio-net header: 10 bytes in legacy mode without MRG_RXBUF */
#define VNET_HDR_WIRE   10

/* Device status bits */
#define VTSTS_ACKNOWLEDGE     0x01
#define VTSTS_DRIVER          0x02
#define VTSTS_DRIVER_OK       0x04
#define VTSTS_FEATURES_OK     0x08
#define VTSTS_DEVICE_NEEDS_RESET 0x40
#define VTSTS_FAILED          0x80

/* Virtio device IDs */
#define VTDEV_NET             1

/* Virtio-net feature bits (low word, selector 0) */
#define VTNET_F_MAC           (1u << 5)
/* Virtio v1 feature bits (high word, selector 1, bit 0 = overall bit 32) */
#define VT_F_VERSION_1        (1u << 0)

/* Virtio descriptor flags */
#define VTDESC_F_NEXT         0x0001
#define VTDESC_F_WRITE        0x0002  /* device-writable (for RX) */

/* Virtqueue size */
#define VQ_SIZE               16

/* ------------------------------------------------------------------ */
/* Virtio-net header (virtio 1.0, §5.1.6)                              */
/* ------------------------------------------------------------------ */
struct __attribute__((packed)) vnet_hdr
{
  l4_uint8_t  flags;
  l4_uint8_t  gso_type;
  l4_uint16_t hdr_len;
  l4_uint16_t gso_size;
  l4_uint16_t csum_start;
  l4_uint16_t csum_offset;
  l4_uint16_t num_buffers;
};

/* ------------------------------------------------------------------ */
/* Virtqueue descriptor, available ring, used ring                     */
/* ------------------------------------------------------------------ */
struct vq_desc
{
  l4_uint64_t addr;
  l4_uint32_t len;
  l4_uint16_t flags;
  l4_uint16_t next;
} __attribute__((packed));

struct vq_avail
{
  l4_uint16_t flags;
  l4_uint16_t idx;
  l4_uint16_t ring[VQ_SIZE];
  l4_uint16_t used_event;
} __attribute__((packed));

struct vq_used_elem
{
  l4_uint32_t id;
  l4_uint32_t len;
} __attribute__((packed));

struct vq_used
{
  l4_uint16_t    flags;
  l4_uint16_t    idx;
  vq_used_elem   ring[VQ_SIZE];
  l4_uint16_t    avail_event;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* DMA layout                                                           */
/*                                                                      */
/* Each virtnet_q page is compatible with BOTH legacy and modern:       */
/*   offset 0   : descriptor table (VQ_SIZE × 16 B = 256 B)            */
/*   offset 256 : available ring   (38 B)                               */
/*   offset 320 : used ring        (134 B)   ← align(294, 64) = 320    */
/*   offset 454+: padding to fill the page                              */
/*                                                                      */
/* Legacy (v1): QUEUE_PFN = page phys >> 12, QUEUE_ALIGN = 64          */
/* Modern (v2): absolute desc/avail/used addresses                      */
/* ------------------------------------------------------------------ */

struct virtnet_q
{
  vq_desc  desc[VQ_SIZE];    /* offset   0, 256 B */
  vq_avail avail;            /* offset 256,  38 B */
  l4_uint8_t _pad[Q_USED_OFF
    - sizeof(vq_desc) * VQ_SIZE
    - sizeof(vq_avail)];     /* offset 294,  26 B → reach 320 */
  vq_used  used;             /* offset 320, 134 B */
  l4_uint8_t _tail[L4_PAGESIZE - Q_USED_OFF - sizeof(vq_used)];
} __attribute__((packed, aligned(4096)));

struct virtnet_dma
{
  virtnet_q  rx_q;                   /* page 0 (4096 B) */
  virtnet_q  tx_q;                   /* page 1 (4096 B) */
  l4_uint8_t rx_buf[VQ_SIZE][2048];  /* 16 × 2048 B     */
  l4_uint8_t tx_buf[2048];           /* TX staging       */
} __attribute__((packed, aligned(4096)));

/* ------------------------------------------------------------------ */
/* Driver state                                                         */
/* ------------------------------------------------------------------ */
static volatile l4_uint8_t *mmio_base = nullptr;
static struct virtnet_dma   *dma      = nullptr;
static l4_uint64_t           dma_phys = 0;

/* Pending RX descriptors returned to the driver (per-slot tracking) */
static l4_uint16_t rx_last_used = 0; /* last processed used-ring idx */
static l4_uint16_t tx_last_used = 0;
static l4_uint16_t tx_avail_idx = 0; /* next TX avail slot */

static struct netif  vn_netif;

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                         */
/* ------------------------------------------------------------------ */
static inline l4_uint32_t vreg_r(unsigned off)
{ return *reinterpret_cast<volatile l4_uint32_t const *>(mmio_base + off); }

static inline void vreg_w(unsigned off, l4_uint32_t v)
{ *reinterpret_cast<volatile l4_uint32_t *>(mmio_base + off) = v; }

/* ------------------------------------------------------------------ */
/* Virtio feature negotiation helper                                    */
/* ------------------------------------------------------------------ */
static l4_uint32_t dev_features(unsigned sel)
{
  vreg_w(VTMMIO_DEV_FEAT_SEL, sel);
  return vreg_r(VTMMIO_DEV_FEAT);
}

static void accept_features(unsigned sel, l4_uint32_t bits)
{
  vreg_w(VTMMIO_DRV_FEAT_SEL, sel);
  vreg_w(VTMMIO_DRV_FEAT, bits);
}

/* ------------------------------------------------------------------ */
/* lwIP netif output callback (called from tcpip_thread)               */
/* ------------------------------------------------------------------ */
static err_t vnet_output(struct netif */*netif*/, struct pbuf *p)
{
  l4_uint16_t pkt_len = (l4_uint16_t)(VNET_HDR_WIRE + p->tot_len);
  if (pkt_len > (l4_uint16_t)sizeof(dma->tx_buf))
    return ERR_MEM;

  memset(dma->tx_buf, 0, VNET_HDR_WIRE);

  l4_uint8_t *payload = dma->tx_buf + VNET_HDR_WIRE;
  l4_uint16_t copied  = pbuf_copy_partial(p, payload, p->tot_len, 0);
  (void)copied;

  l4_uint16_t slot = tx_avail_idx & (VQ_SIZE - 1);

  /* Fill TX descriptor */
  dma->tx_q.desc[slot].addr  = dma_phys
    + __builtin_offsetof(struct virtnet_dma, tx_buf);
  dma->tx_q.desc[slot].len   = pkt_len;
  dma->tx_q.desc[slot].flags = 0;   /* device-readable */
  dma->tx_q.desc[slot].next  = 0;

  /* Add to TX available ring */
  dma->tx_q.avail.ring[tx_avail_idx & (VQ_SIZE - 1)] = slot;
  __sync_synchronize();
  dma->tx_q.avail.idx = (l4_uint16_t)(tx_avail_idx + 1);
  __sync_synchronize();

  /* Notify device: queue 1 */
  vreg_w(VTMMIO_QUEUE_NOTIFY, 1);
  ++tx_avail_idx;

  /* Poll used ring until the TX descriptor is consumed */
  while (dma->tx_q.used.idx == tx_last_used)
    ; /* busy-wait; adequate for test */

  /* Acknowledge used TX descriptors */
  vreg_w(VTMMIO_IRQ_ACK, vreg_r(VTMMIO_IRQ_STATUS));
  while (tx_last_used != dma->tx_q.used.idx)
    ++tx_last_used;

  return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* RX polling thread                                                    */
/* ------------------------------------------------------------------ */
static void *rx_thread(void *arg)
{
  struct netif *nif = reinterpret_cast<struct netif *>(arg);

  for (;;)
    {
      /* Poll used ring of queue 0 */
      while (dma->rx_q.used.idx == rx_last_used)
        l4_sleep(1); /* yield 1 ms */

      /* Acknowledge interrupt */
      vreg_w(VTMMIO_IRQ_ACK, vreg_r(VTMMIO_IRQ_STATUS));

      while (rx_last_used != dma->rx_q.used.idx)
        {
          vq_used_elem &ue = dma->rx_q.used.ring[rx_last_used & (VQ_SIZE - 1)];
          l4_uint16_t   id  = (l4_uint16_t)ue.id;
          l4_uint32_t   len = ue.len;

          if (len > VNET_HDR_WIRE)
            {
              /* Skip 10-byte on-wire header (legacy, no MRG_RXBUF) */
              l4_uint8_t *frame = dma->rx_buf[id] + VNET_HDR_WIRE;
              l4_uint16_t  flen = (l4_uint16_t)(len - VNET_HDR_WIRE);

              struct pbuf *pb = pbuf_alloc(PBUF_RAW, flen, PBUF_POOL);
              if (pb)
                {
                  pbuf_take(pb, frame, flen);
                  /* tcpip_input() must be called WITHOUT core lock held —
                     it posts to tcpip_thread's mailbox internally */
                  if (nif->input(pb, nif) != ERR_OK)
                    pbuf_free(pb);
                }
            }

          /* Re-arm RX descriptor */
          dma->rx_q.desc[id].addr  = dma_phys
            + __builtin_offsetof(struct virtnet_dma, rx_buf)
            + (l4_uint64_t)id * 2048;
          dma->rx_q.desc[id].len   = 2048;
          dma->rx_q.desc[id].flags = VTDESC_F_WRITE;
          dma->rx_q.desc[id].next  = 0;

          __sync_synchronize();
          dma->rx_q.avail.ring[dma->rx_q.avail.idx & (VQ_SIZE - 1)] = id;
          dma->rx_q.avail.idx = (l4_uint16_t)(dma->rx_q.avail.idx + 1);
          __sync_synchronize();
          vreg_w(VTMMIO_QUEUE_NOTIFY, 0); /* notify RX queue */

          ++rx_last_used;
        }
    }
  return nullptr;
}

/* ------------------------------------------------------------------ */
/* lwIP netif init callback                                             */
/* ------------------------------------------------------------------ */
static err_t vnet_init(struct netif *nif)
{
  nif->name[0] = 'v';
  nif->name[1] = 'n';
  nif->output     = etharp_output;
  nif->linkoutput = vnet_output;
  nif->mtu        = 1500;
  nif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                  | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;

  /* Read MAC from virtio-net config register */
  for (int i = 0; i < 6; ++i)
    nif->hwaddr[i] = *reinterpret_cast<volatile l4_uint8_t const *>(
                       mmio_base + VTMMIO_CONFIG + i);
  nif->hwaddr_len = 6;

  printf("tcp_server: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
         nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
         nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5]);

  netif_set_default(nif);

  /* Start RX polling thread */
  pthread_t t;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &attr, rx_thread, nif) != 0)
    {
      printf("tcp_server: failed to create RX thread\n");
      return ERR_IF;
    }
  pthread_attr_destroy(&attr);
  return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Virtio-MMIO hardware initialisation  (version 1 legacy + version 2) */
/* ------------------------------------------------------------------ */

/* ---- common: pre-fill RX descriptors ---- */
static void prefill_rx(l4_uint32_t qsz)
{
  l4_uint64_t buf_base = dma_phys
    + __builtin_offsetof(struct virtnet_dma, rx_buf);
  for (l4_uint32_t i = 0; i < qsz; ++i)
    {
      dma->rx_q.desc[i].addr  = buf_base + (l4_uint64_t)i * 2048;
      dma->rx_q.desc[i].len   = 2048;
      dma->rx_q.desc[i].flags = VTDESC_F_WRITE;
      dma->rx_q.desc[i].next  = 0;
      dma->rx_q.avail.ring[i] = (l4_uint16_t)i;
    }
  dma->rx_q.avail.flags = 0;
  dma->rx_q.avail.idx   = (l4_uint16_t)qsz;
  __sync_synchronize();
  vreg_w(VTMMIO_QUEUE_NOTIFY, 0);
}

/* ---- version 1 (legacy) init ---- */
static bool virtio_net_init_v1(struct virtnet_dma *vdma)
{
  printf("virtio_net: legacy protocol (v1)\n");

  /* Driver must tell the device the page size it uses */
  vreg_w(VTMMIO_GUEST_PAGE_SIZE, L4_PAGESIZE);

  /* ACKNOWLEDGE + DRIVER */
  vreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER);
  __sync_synchronize();

  /* Feature negotiation: single 32-bit register, no selector */
  l4_uint32_t feat = vreg_r(VTMMIO_DEV_FEAT) & VTNET_F_MAC;
  vreg_w(VTMMIO_DRV_FEAT, feat);

  /* Setup RX queue (queue 0) via QUEUE_PFN */
  vreg_w(VTMMIO_QUEUE_SEL, 0);
  l4_uint32_t qmax = vreg_r(VTMMIO_QUEUE_NUM_MAX);
  l4_uint32_t qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  vreg_w(VTMMIO_QUEUE_NUM, qsz);
  vreg_w(VTMMIO_QUEUE_ALIGN, QUEUE_ALIGN_V1);
  /* PFN = physical address of queue page >> PAGE_SHIFT */
  l4_uint32_t rx_pfn = (l4_uint32_t)(
    (dma_phys + __builtin_offsetof(struct virtnet_dma, rx_q)) >> L4_PAGESHIFT);
  vreg_w(VTMMIO_QUEUE_PFN, rx_pfn);
  printf("virtio_net: RX queue pfn=0x%x (phys=0x%llx)\n",
         rx_pfn, (unsigned long long)((l4_uint64_t)rx_pfn << L4_PAGESHIFT));

  /* Setup TX queue (queue 1) */
  vreg_w(VTMMIO_QUEUE_SEL, 1);
  qmax = vreg_r(VTMMIO_QUEUE_NUM_MAX);
  qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  vreg_w(VTMMIO_QUEUE_NUM, qsz);
  vreg_w(VTMMIO_QUEUE_ALIGN, QUEUE_ALIGN_V1);
  l4_uint32_t tx_pfn = (l4_uint32_t)(
    (dma_phys + __builtin_offsetof(struct virtnet_dma, tx_q)) >> L4_PAGESHIFT);
  vreg_w(VTMMIO_QUEUE_PFN, tx_pfn);
  printf("virtio_net: TX queue pfn=0x%x\n", tx_pfn);

  dma->tx_q.avail.flags = 0;
  dma->tx_q.avail.idx   = 0;

  /* DRIVER_OK before prefill: QEMU ignores QUEUE_NOTIFY sent before
     the device is active. Set DRIVER_OK first, then fill the RX ring. */
  vreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_DRIVER_OK);
  __sync_synchronize();

  prefill_rx(qsz);

  (void)vdma;
  printf("virtio_net: device ready (v1 legacy)\n");
  return true;
}

/* ---- version 2 (modern) init ---- */
static bool virtio_net_init_v2(struct virtnet_dma */*vdma*/)
{
  printf("virtio_net: modern protocol (v2)\n");

  /* ACKNOWLEDGE + DRIVER */
  vreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER);

  /* Feature negotiation with selector */
  l4_uint32_t feat0 = dev_features(0) & VTNET_F_MAC;
  l4_uint32_t feat1 = dev_features(1) & VT_F_VERSION_1;
  if (!(feat1 & VT_F_VERSION_1))
    { printf("virtio_net: device does not support VERSION_1\n"); return false; }
  accept_features(0, feat0);
  accept_features(1, feat1);

  /* FEATURES_OK */
  vreg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_FEATURES_OK);
  __sync_synchronize();
  if (!(vreg_r(VTMMIO_STATUS) & VTSTS_FEATURES_OK))
    { printf("virtio_net: FEATURES_OK rejected\n"); return false; }

  /* RX queue (queue 0) */
  vreg_w(VTMMIO_QUEUE_SEL, 0);
  l4_uint32_t qmax = vreg_r(VTMMIO_QUEUE_NUM_MAX);
  l4_uint32_t qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  vreg_w(VTMMIO_QUEUE_NUM, qsz);

  l4_uint64_t rx_desc  = dma_phys + __builtin_offsetof(virtnet_dma, rx_q)
                         + __builtin_offsetof(virtnet_q, desc);
  l4_uint64_t rx_avail = dma_phys + __builtin_offsetof(virtnet_dma, rx_q)
                         + __builtin_offsetof(virtnet_q, avail);
  l4_uint64_t rx_used  = dma_phys + __builtin_offsetof(virtnet_dma, rx_q)
                         + Q_USED_OFF;

  vreg_w(VTMMIO_QUEUE_DESC_LO, (l4_uint32_t)(rx_desc  & 0xffffffff));
  vreg_w(VTMMIO_QUEUE_DESC_HI, (l4_uint32_t)(rx_desc  >> 32));
  vreg_w(VTMMIO_QUEUE_DRV_LO,  (l4_uint32_t)(rx_avail & 0xffffffff));
  vreg_w(VTMMIO_QUEUE_DRV_HI,  (l4_uint32_t)(rx_avail >> 32));
  vreg_w(VTMMIO_QUEUE_DEV_LO,  (l4_uint32_t)(rx_used  & 0xffffffff));
  vreg_w(VTMMIO_QUEUE_DEV_HI,  (l4_uint32_t)(rx_used  >> 32));
  vreg_w(VTMMIO_QUEUE_READY, 1);

  /* TX queue (queue 1) */
  vreg_w(VTMMIO_QUEUE_SEL, 1);
  qmax = vreg_r(VTMMIO_QUEUE_NUM_MAX);
  qsz  = (qmax >= VQ_SIZE) ? VQ_SIZE : qmax;
  vreg_w(VTMMIO_QUEUE_NUM, qsz);

  l4_uint64_t tx_desc  = dma_phys + __builtin_offsetof(virtnet_dma, tx_q)
                         + __builtin_offsetof(virtnet_q, desc);
  l4_uint64_t tx_avail = dma_phys + __builtin_offsetof(virtnet_dma, tx_q)
                         + __builtin_offsetof(virtnet_q, avail);
  l4_uint64_t tx_used  = dma_phys + __builtin_offsetof(virtnet_dma, tx_q)
                         + Q_USED_OFF;

  vreg_w(VTMMIO_QUEUE_DESC_LO, (l4_uint32_t)(tx_desc  & 0xffffffff));
  vreg_w(VTMMIO_QUEUE_DESC_HI, (l4_uint32_t)(tx_desc  >> 32));
  vreg_w(VTMMIO_QUEUE_DRV_LO,  (l4_uint32_t)(tx_avail & 0xffffffff));
  vreg_w(VTMMIO_QUEUE_DRV_HI,  (l4_uint32_t)(tx_avail >> 32));
  vreg_w(VTMMIO_QUEUE_DEV_LO,  (l4_uint32_t)(tx_used  & 0xffffffff));
  vreg_w(VTMMIO_QUEUE_DEV_HI,  (l4_uint32_t)(tx_used  >> 32));
  vreg_w(VTMMIO_QUEUE_READY, 1);

  prefill_rx(qsz);
  dma->tx_q.avail.flags = 0;
  dma->tx_q.avail.idx   = 0;

  /* DRIVER_OK */
  vreg_w(VTMMIO_STATUS,
         VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_FEATURES_OK | VTSTS_DRIVER_OK);
  __sync_synchronize();
  printf("virtio_net: device ready (v2 modern)\n");
  return true;
}

/* ---- dispatcher ---- */
static bool virtio_net_init(l4_addr_t mmio_virt, l4_uint64_t phys_base,
                            struct virtnet_dma *vdma)
{
  mmio_base = reinterpret_cast<volatile l4_uint8_t *>(mmio_virt);
  dma       = vdma;
  dma_phys  = phys_base;

  l4_uint32_t magic   = vreg_r(VTMMIO_MAGIC);
  l4_uint32_t version = vreg_r(VTMMIO_VERSION);
  l4_uint32_t devid   = vreg_r(VTMMIO_DEVICE_ID);

  if (magic != 0x74726976u)
    { printf("virtio_net: bad magic 0x%08x\n", magic); return false; }
  if (devid != VTDEV_NET)
    { printf("virtio_net: device_id=%u, expected %u\n", devid, VTDEV_NET); return false; }
  if (version != 1 && version != 2)
    { printf("virtio_net: unsupported version %u\n", version); return false; }

  printf("virtio_net: found virtio-net device (v%u)\n", version);

  /* Reset */
  vreg_w(VTMMIO_STATUS, 0);
  __sync_synchronize();

  return (version == 1) ? virtio_net_init_v1(vdma)
                        : virtio_net_init_v2(vdma);
}

/* ------------------------------------------------------------------ */
/* MMIO mapping via sigma0 scan                                         */
/*                                                                      */
/* QEMU ARM virt places virtio-mmio slots at:                           */
/*   0x0a000000 + slot * 0x200   (slot 0..31)                           */
/* The slot used for a given -device depends on QEMU version/order.     */
/* We map all 4 pages (covering 32 slots) and scan for device_id == 1. */
/* ------------------------------------------------------------------ */
static bool map_virtio_mmio(l4_addr_t *virt_out)
{
  constexpr l4_addr_t VIRTIO_BASE = 0x0a000000UL;
  constexpr unsigned  NUM_PAGES   = 4;            /* 4 × 4KB = 32 slots */
  constexpr l4_size_t MAP_SIZE    = NUM_PAGES * L4_PAGESIZE;

  auto sigma0 = L4Re::Env::env()->get_cap<void>("sigma0");
  if (!sigma0.is_valid())
    { printf("tcp_server: sigma0 cap not found\n"); return false; }

  /* Reserve contiguous virtual window */
  l4_addr_t vbase = 0;
  if (L4Re::Env::env()->rm()->reserve_area(
        &vbase, MAP_SIZE, L4Re::Rm::F::Search_addr, L4_PAGESHIFT) < 0)
    { printf("tcp_server: reserve MMIO area failed\n"); return false; }

  /* Map all 4 pages via sigma0 iomem (uncached) */
  for (unsigned p = 0; p < NUM_PAGES; ++p)
    {
      l4_addr_t phys = VIRTIO_BASE + (l4_addr_t)p * L4_PAGESIZE;
      l4_addr_t virt = vbase       + (l4_addr_t)p * L4_PAGESIZE;
      int r = l4sigma0_map_iomem(sigma0.cap(), phys, virt, L4_PAGESIZE,
                                 0 /* uncached */);
      if (r < 0)
        {
          printf("tcp_server: sigma0 iomem map failed page %u: %d\n", p, r);
          L4Re::Env::env()->rm()->free_area(vbase);
          return false;
        }
    }

  /* Scan all 32 slots */
  for (unsigned i = 0; i < 32; ++i)
    {
      volatile l4_uint32_t *regs =
        reinterpret_cast<volatile l4_uint32_t *>(vbase + i * 0x200u);

      l4_uint32_t magic = regs[0];          /* offset 0x000 */
      l4_uint32_t devid = regs[0x008 / 4];  /* offset 0x008 */

      if (magic != 0x74726976u)
        continue;   /* not a valid virtio-mmio slot */

      printf("tcp_server: slot %2u phys=0x%08lx device_id=%u\n",
             i, VIRTIO_BASE + i * 0x200u, devid);

      if (devid == VTDEV_NET)
        {
          printf("tcp_server: virtio-net found at slot %u (phys 0x%08lx)\n",
                 i, VIRTIO_BASE + i * 0x200u);
          *virt_out = (l4_addr_t)regs;
          return true;
        }
    }

  printf("tcp_server: virtio-net not found in any MMIO slot\n");
  L4Re::Env::env()->rm()->free_area(vbase);
  return false;
}

/* ------------------------------------------------------------------ */
/* DMA memory allocation                                                */
/*                                                                      */
/* QEMU ARM virt has no IOMMU: physical address == bus address.         */
/* Strategy:                                                            */
/*   1. Create a Dma_space and call associate(Phys_space) to enable     */
/*      the bypass (no-IOMMU) mapper – avoids assign_dma_domain.        */
/*   2. Alloc a Continuous+Pinned dataspace from moe.                   */
/*   3. dma_space->map() asks moe for the physical start address.       */
/* ------------------------------------------------------------------ */
static bool alloc_dma_mem(struct virtnet_dma **dma_out,
                          l4_uint64_t         *phys_out)
{
  constexpr l4_size_t DMA_SIZE   = sizeof(struct virtnet_dma);
  constexpr l4_size_t ALLOC_SIZE =
    (DMA_SIZE + L4_PAGESIZE - 1) & ~(l4_size_t)(L4_PAGESIZE - 1);

  /* --- 1. Create DMA space and associate with physical-bypass mode --- */
  auto dma_space = L4Re::chkcap(
    L4Re::Util::make_unique_cap<L4Re::Dma_space>(),
    "alloc DMA space cap");
  L4Re::chksys(
    L4Re::Env::env()->user_factory()->create(dma_space.get()),
    "create DMA space");
  /* Phys_space flag: moe uses a Phys_mapper (bypass, no IOMMU needed) */
  L4Re::chksys(
    dma_space->associate(L4::Ipc::Cap<L4::Task>(),
                         L4Re::Dma_space::Phys_space),
    "associate DMA space (phys bypass)");

  /* --- 2. Allocate physically contiguous, pinned dataspace --- */
  auto ds = L4Re::chkcap(
    L4Re::Util::make_unique_cap<L4Re::Dataspace>(),
    "alloc DMA DS cap");
  L4Re::chksys(
    L4Re::Env::env()->mem_alloc()->alloc(
      ALLOC_SIZE, ds.get(),
      L4Re::Mem_alloc::Continuous | L4Re::Mem_alloc::Pinned),
    "alloc DMA memory");

  /* --- 3. Map into our virtual address space --- */
  l4_addr_t vaddr = 0;
  L4Re::chksys(
    L4Re::Env::env()->rm()->attach(
      &vaddr, ALLOC_SIZE,
      L4Re::Rm::F::Search_addr | L4Re::Rm::F::Eager_map | L4Re::Rm::F::RW,
      L4::Ipc::make_cap_rw(ds.get())),
    "attach DMA memory");
  memset(reinterpret_cast<void *>(vaddr), 0, ALLOC_SIZE);

  /* --- 4. Get physical (bus) address via DMA space map --- */
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

  printf("tcp_server: DMA buffer @ virt=%p phys=0x%llx size=%zu\n",
         (void *)vaddr, (unsigned long long)dma_addr, ALLOC_SIZE);

  /* Caps released here; moe keeps the memory alive via the mapping */
  (void)ds.release();
  (void)dma_space.release();

  *dma_out  = reinterpret_cast<struct virtnet_dma *>(vaddr);
  *phys_out = (l4_uint64_t)dma_addr;
  return true;
}

/* ------------------------------------------------------------------ */
/* IP configuration from environment                                    */
/* ------------------------------------------------------------------ */
static void configure_ip()
{
  const char *cfg = getenv("IFCONFIG_IP4_vn0");
  if (!cfg)
    {
      printf("tcp_server: IFCONFIG_IP4_vn0 not set, using 10.0.2.15/24 via 10.0.2.2\n");
      cfg = "10.0.2.15/24 via 10.0.2.2";
    }

  ip4_addr_t ip, nm, gw;
  unsigned prefix = 24;
  char ip_str[20] = {}, gw_str[20] = {};
  unsigned ip0, ip1, ip2, ip3, gw0, gw1, gw2, gw3;

  int n = sscanf(cfg, "%u.%u.%u.%u/%u via %u.%u.%u.%u",
                 &ip0, &ip1, &ip2, &ip3, &prefix,
                 &gw0, &gw1, &gw2, &gw3);

  /* Use ip4addr_aton() which correctly uses lwip_htonl() at runtime,
     avoiding the compile-time PP_HTONL byte-order bug on this platform. */
  snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip0, ip1, ip2, ip3);
  ip4addr_aton(ip_str, &ip);

  if (n >= 5)
    ip4_addr_set_u32(&nm,
      lwip_htonl(~0UL << (32 - (prefix > 32 ? 32 : prefix))));
  else
    ip4addr_aton("255.255.255.0", &nm);

  if (n >= 9)
    {
      snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", gw0, gw1, gw2, gw3);
      ip4addr_aton(gw_str, &gw);
    }
  else
    ip4addr_aton("10.0.2.2", &gw);

  LOCK_TCPIP_CORE();
  netif_set_addr(&vn_netif, &ip, &nm, &gw);
  UNLOCK_TCPIP_CORE();

  printf("tcp_server: IP %s", ip4addr_ntoa(&ip));
  printf("  netmask %s", ip4addr_ntoa(&nm));
  printf("  gw %s\n", ip4addr_ntoa(&gw));
}

/* ------------------------------------------------------------------ */
/* TCP echo server                                                       */
/* ------------------------------------------------------------------ */
static void handle_client(int client_fd, struct sockaddr_in *cli)
{
  printf("tcp_server: client connected from %s:%d\n",
         inet_ntoa(cli->sin_addr), ntohs(cli->sin_port));

  /* Send greeting */
  const char *banner = "Hello from TuringOS TCP server!\r\n";
  send(client_fd, banner, strlen(banner), 0);

  char buf[512];
  ssize_t n;
  while ((n = recv(client_fd, buf, sizeof(buf) - 1, 0)) > 0)
    {
      buf[n] = '\0';
      printf("tcp_server: rx %zd bytes: %s", n, buf);
      /* Echo back */
      send(client_fd, buf, (size_t)n, 0);
    }

  printf("tcp_server: client disconnected\n");
  close(client_fd);
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main()
{
  printf("\n=== TuringOS TCP Echo Server (port %d) ===\n\n", CONFIG_TCP_PORT);

  /* ---- Get vbus capability ---- */
  auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (!vbus.is_valid())
    {
      printf("tcp_server: 'vbus' capability not found\n");
      return 1;
    }

  /* ---- Allocate DMA buffer ---- */
  struct virtnet_dma *vdma = nullptr;
  l4_uint64_t dma_bus_addr = 0;
  if (!alloc_dma_mem(&vdma, &dma_bus_addr))
    return 1;

  /* ---- Map virtio MMIO registers (scan all slots via sigma0) ---- */
  l4_addr_t mmio_virt = 0;
  if (!map_virtio_mmio(&mmio_virt))
    return 1;

  /* ---- Initialise virtio-net hardware ---- */
  if (!virtio_net_init(mmio_virt, dma_bus_addr, vdma))
    return 1;

  /* lwIP TCP/IP stack is already initialized by the lwip library constructor
     (l4libinit.cc calls tcpip_init before main). Do NOT call tcpip_init()
     again — it would double-initialize and corrupt lwIP state. */

  /* ---- Add custom netif ---- */
  ip4_addr_t ip_zero;
  ip4_addr_set_zero(&ip_zero);
  if (netifapi_netif_add(&vn_netif, &ip_zero, &ip_zero, &ip_zero,
                         nullptr, vnet_init, tcpip_input) != ERR_OK)
    {
      printf("tcp_server: netif_add failed\n");
      return 1;
    }
  LOCK_TCPIP_CORE();
  netif_set_up(&vn_netif);
  netif_set_default(&vn_netif);
  UNLOCK_TCPIP_CORE();

  /* ---- Configure IP address ---- */
  configure_ip();

  /* ---- Open TCP listening socket ---- */
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0)
    { perror("socket"); return 1; }

  int on = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(CONFIG_TCP_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    { perror("bind"); return 1; }

  if (listen(srv, 4) < 0)
    { perror("listen"); return 1; }

  printf("tcp_server: listening on port %d\n", CONFIG_TCP_PORT);
  printf("tcp_server: from host, connect with:\n");
  printf("  python3 tools/tcp_client.py --host 10.0.2.15 --port %d\n",
         CONFIG_TCP_PORT);

  /* ---- Accept loop ---- */
  for (;;)
    {
      struct sockaddr_in cli{};
      socklen_t cli_len = sizeof(cli);
      int cfd = accept(srv, reinterpret_cast<struct sockaddr *>(&cli), &cli_len);
      if (cfd < 0)
        {
          perror("accept");
          continue;
        }
      handle_client(cfd, &cli);
    }
}
