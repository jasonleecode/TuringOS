/*
 * virtio-serial — VirtIO serial (device ID 3) driver as L4::Vcon IPC server
 *
 * Architecture:
 *   QEMU virtio-serial (virtio-mmio-bus.2)
 *         ↓  vbus/MMIO
 *   this server  →  implements L4::Vcon IPC protocol
 *         ↓  vcon capability
 *   uart-test (or any Vcon client)
 *
 * Clients use l4_vcon_write() / l4_vcon_read() only.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <l4/re/env>
#include <l4/re/rm>
#include <l4/re/mem_alloc>
#include <l4/re/dma_space>
#include <l4/re/util/unique_cap>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>
#include <l4/re/util/vcon_svr>
#include <l4/re/util/icu_svr>
#include <l4/re/error_helper>
#include <l4/sys/consts.h>
#include <l4/sys/vcon>
#include <l4/sys/vcon.h>
#include <l4/vbus/vbus>
#include <l4/util/util.h>

/* ------------------------------------------------------------------ */
/* VirtIO MMIO register offsets                                        */
/* ------------------------------------------------------------------ */
enum : unsigned {
  VTMMIO_MAGIC           = 0x000,
  VTMMIO_VERSION         = 0x004,
  VTMMIO_DEVICE_ID       = 0x008,
  VTMMIO_DEV_FEAT        = 0x010,
  VTMMIO_DEV_FEAT_SEL    = 0x014,
  VTMMIO_DRV_FEAT        = 0x020,
  VTMMIO_DRV_FEAT_SEL    = 0x024,
  VTMMIO_QUEUE_SEL       = 0x030,
  VTMMIO_QUEUE_NUM_MAX   = 0x034,
  VTMMIO_QUEUE_NUM       = 0x038,
  VTMMIO_QUEUE_READY     = 0x044,  /* v2 only */
  VTMMIO_QUEUE_NOTIFY    = 0x050,
  VTMMIO_IRQ_STATUS      = 0x060,
  VTMMIO_IRQ_ACK         = 0x064,
  VTMMIO_STATUS          = 0x070,
  VTMMIO_QUEUE_DESC_LO   = 0x080,  /* v2 only */
  VTMMIO_QUEUE_DESC_HI   = 0x084,
  VTMMIO_QUEUE_DRV_LO    = 0x090,
  VTMMIO_QUEUE_DRV_HI    = 0x094,
  VTMMIO_QUEUE_DEV_LO    = 0x0a0,
  VTMMIO_QUEUE_DEV_HI    = 0x0a4,
  VTMMIO_GUEST_PAGE_SIZE = 0x028,  /* v1 only */
  VTMMIO_QUEUE_ALIGN     = 0x03c,  /* v1 only */
  VTMMIO_QUEUE_PFN       = 0x040,  /* v1 only */
};

enum : l4_uint32_t { VT_MAGIC = 0x74726976u };
enum : l4_uint32_t { VT_DEV_SERIAL = 3 };
enum : l4_uint32_t {
  VTSTS_ACKNOWLEDGE = 0x01,
  VTSTS_DRIVER      = 0x02,
  VTSTS_DRIVER_OK   = 0x04,
  VTSTS_FEATURES_OK = 0x08,
};
enum : l4_uint32_t { VT_F_VERSION_1 = 1u << 0 };
enum : l4_uint16_t { VTDESC_F_WRITE = 0x0002 };

/* ------------------------------------------------------------------ */
/* VirtQueue structures                                                */
/* ------------------------------------------------------------------ */
enum : unsigned { VQ_SIZE   = 16  };
enum : unsigned { VQ_ALIGN  = 64u };

/* Used ring offset: after desc table + avail ring, aligned to VQ_ALIGN */
enum : unsigned {
  VQ_DESC_BYTES  = VQ_SIZE * 16,
  VQ_AVAIL_BYTES = 4 + VQ_SIZE * 2 + 2,
  VQ_USED_OFF    = (VQ_DESC_BYTES + VQ_AVAIL_BYTES + VQ_ALIGN - 1u)
                   & ~(VQ_ALIGN - 1u),
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
  l4_uint16_t  flags;
  l4_uint16_t  idx;
  vq_used_elem ring[VQ_SIZE];
  l4_uint16_t  avail_event;
};

/* One page-aligned queue block (desc + avail + pad + used) */
struct __attribute__((packed, aligned(4096))) virtq {
  vq_desc  desc[VQ_SIZE];
  vq_avail avail;
  l4_uint8_t _pad[VQ_USED_OFF - VQ_DESC_BYTES - VQ_AVAIL_BYTES];
  vq_used  used;
  l4_uint8_t _tail[L4_PAGESIZE - VQ_USED_OFF - sizeof(vq_used)];
};

enum : unsigned { RX_BUF_SIZE = 256 };
enum : unsigned { TX_BUF_SIZE = 512 };

/* DMA region: rxq + txq + rx buffers + tx buffer */
struct __attribute__((packed, aligned(4096))) uart_dma {
  virtq      rxq;
  virtq      txq;
  l4_uint8_t rx_buf[VQ_SIZE][RX_BUF_SIZE];
  l4_uint8_t tx_buf[TX_BUF_SIZE];
};

/* ------------------------------------------------------------------ */
/* VirtIO serial hardware driver class                                 */
/* ------------------------------------------------------------------ */

class Virtio_serial_svr
: public L4::Epiface_t<Virtio_serial_svr, L4::Vcon>
, public L4Re::Util::Icu_cap_array_svr<Virtio_serial_svr>
, public L4Re::Util::Vcon_svr<Virtio_serial_svr>
{
public:
  /* L4::Vcon inherits L4::Icu; Icu_cap_array_svr provides that implementation.
   * We advertise 1 IRQ line (the vcon "key interrupt" for read-ready events). */
  Virtio_serial_svr() : Icu_cap_array_svr(1, &_irq) {}

  bool init(l4_addr_t mmio_virt, l4_uint64_t dma_phys, uart_dma *dma);

  /* --- Vcon_svr interface --- */
  void vcon_write(const char *buf, unsigned size) noexcept;
  unsigned vcon_read(char *buf, unsigned size) noexcept;

  int vcon_set_attr(l4_vcon_attr_t const *) noexcept { return -L4_EOK; }
  int vcon_get_attr(l4_vcon_attr_t *attr) noexcept
  {
    attr->i_flags = attr->o_flags = attr->l_flags = 0;
    return -L4_EOK;
  }

private:
  Icu_cap_array_svr::Irq _irq;
  /* MMIO register helpers */
  l4_uint32_t reg_r(unsigned off) const
  { return reinterpret_cast<volatile l4_uint32_t const *>(_mmio)[off / 4]; }

  void reg_w(unsigned off, l4_uint32_t v)
  { reinterpret_cast<volatile l4_uint32_t *>(_mmio)[off / 4] = v; }

  bool init_v2();
  bool init_v1();
  void prefill_rx();

  l4_addr_t   _mmio = 0;
  uart_dma   *_dma  = nullptr;
  l4_uint64_t _phys = 0;

  l4_uint16_t _rx_last     = 0;
  l4_uint16_t _tx_avail    = 0;
  l4_uint16_t _tx_last     = 0;
};

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void Virtio_serial_svr::prefill_rx()
{
  l4_uint64_t base = _phys + __builtin_offsetof(uart_dma, rx_buf);
  for (unsigned i = 0; i < VQ_SIZE; ++i)
    {
      _dma->rxq.desc[i].addr  = base + i * RX_BUF_SIZE;
      _dma->rxq.desc[i].len   = RX_BUF_SIZE;
      _dma->rxq.desc[i].flags = VTDESC_F_WRITE;
      _dma->rxq.desc[i].next  = 0;
      _dma->rxq.avail.ring[i] = (l4_uint16_t)i;
    }
  _dma->rxq.avail.flags = 0;
  _dma->rxq.avail.idx   = VQ_SIZE;
  __sync_synchronize();
  reg_w(VTMMIO_QUEUE_NOTIFY, 0);
}

bool Virtio_serial_svr::init_v2()
{
  reg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER);

  reg_w(VTMMIO_DEV_FEAT_SEL, 1);
  l4_uint32_t feat1 = reg_r(VTMMIO_DEV_FEAT);
  if (!(feat1 & VT_F_VERSION_1))
    {
      printf("[vserial] device does not support VERSION_1\n");
      return false;
    }
  reg_w(VTMMIO_DRV_FEAT_SEL, 0); reg_w(VTMMIO_DRV_FEAT, 0);
  reg_w(VTMMIO_DRV_FEAT_SEL, 1); reg_w(VTMMIO_DRV_FEAT, VT_F_VERSION_1);

  reg_w(VTMMIO_STATUS,
        VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_FEATURES_OK);
  __sync_synchronize();
  if (!(reg_r(VTMMIO_STATUS) & VTSTS_FEATURES_OK))
    {
      printf("[vserial] FEATURES_OK not accepted\n");
      return false;
    }

  auto setup_queue = [this](unsigned qidx, l4_uint64_t q_pa, unsigned qsz)
    {
      reg_w(VTMMIO_QUEUE_SEL, qidx);
      unsigned qmax = reg_r(VTMMIO_QUEUE_NUM_MAX);
      if (qsz > qmax) qsz = qmax;
      reg_w(VTMMIO_QUEUE_NUM, qsz);
      l4_uint64_t desc_pa  = q_pa + __builtin_offsetof(virtq, desc);
      l4_uint64_t avail_pa = q_pa + __builtin_offsetof(virtq, avail);
      l4_uint64_t used_pa  = q_pa + VQ_USED_OFF;
      reg_w(VTMMIO_QUEUE_DESC_LO, (l4_uint32_t)(desc_pa  & 0xffffffff));
      reg_w(VTMMIO_QUEUE_DESC_HI, (l4_uint32_t)(desc_pa  >> 32));
      reg_w(VTMMIO_QUEUE_DRV_LO,  (l4_uint32_t)(avail_pa & 0xffffffff));
      reg_w(VTMMIO_QUEUE_DRV_HI,  (l4_uint32_t)(avail_pa >> 32));
      reg_w(VTMMIO_QUEUE_DEV_LO,  (l4_uint32_t)(used_pa  & 0xffffffff));
      reg_w(VTMMIO_QUEUE_DEV_HI,  (l4_uint32_t)(used_pa  >> 32));
      reg_w(VTMMIO_QUEUE_READY, 1);
    };

  l4_uint64_t rx_pa = _phys + __builtin_offsetof(uart_dma, rxq);
  l4_uint64_t tx_pa = _phys + __builtin_offsetof(uart_dma, txq);
  setup_queue(0, rx_pa, VQ_SIZE);
  setup_queue(1, tx_pa, VQ_SIZE);

  _dma->txq.avail.flags = 0;
  _dma->txq.avail.idx   = 0;

  reg_w(VTMMIO_STATUS,
        VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_FEATURES_OK | VTSTS_DRIVER_OK);
  __sync_synchronize();

  prefill_rx();
  printf("[vserial] virtio-serial ready (v2 modern)\n");
  return true;
}

bool Virtio_serial_svr::init_v1()
{
  reg_w(VTMMIO_GUEST_PAGE_SIZE, L4_PAGESIZE);
  reg_w(VTMMIO_STATUS, VTSTS_ACKNOWLEDGE | VTSTS_DRIVER);
  __sync_synchronize();
  reg_w(VTMMIO_DRV_FEAT, 0);

  auto setup_queue_v1 = [this](unsigned qidx, l4_uint64_t q_pa)
    {
      reg_w(VTMMIO_QUEUE_SEL, qidx);
      unsigned qsz = reg_r(VTMMIO_QUEUE_NUM_MAX);
      if (qsz > VQ_SIZE) qsz = VQ_SIZE;
      reg_w(VTMMIO_QUEUE_NUM, qsz);
      reg_w(VTMMIO_QUEUE_ALIGN, VQ_ALIGN);
      reg_w(VTMMIO_QUEUE_PFN, (l4_uint32_t)(q_pa >> L4_PAGESHIFT));
    };

  l4_uint64_t rx_pa = _phys + __builtin_offsetof(uart_dma, rxq);
  l4_uint64_t tx_pa = _phys + __builtin_offsetof(uart_dma, txq);
  setup_queue_v1(0, rx_pa);
  setup_queue_v1(1, tx_pa);

  _dma->txq.avail.flags = 0;
  _dma->txq.avail.idx   = 0;

  reg_w(VTMMIO_STATUS,
        VTSTS_ACKNOWLEDGE | VTSTS_DRIVER | VTSTS_DRIVER_OK);
  __sync_synchronize();

  prefill_rx();
  printf("[vserial] virtio-serial ready (v1 legacy)\n");
  return true;
}

bool Virtio_serial_svr::init(l4_addr_t mmio_virt,
                             l4_uint64_t dma_phys, uart_dma *dma)
{
  _mmio = mmio_virt;
  _phys = dma_phys;
  _dma  = dma;

  l4_uint32_t magic   = reg_r(VTMMIO_MAGIC);
  l4_uint32_t version = reg_r(VTMMIO_VERSION);
  l4_uint32_t devid   = reg_r(VTMMIO_DEVICE_ID);

  printf("[vserial] magic=0x%08x version=%u device_id=%u\n",
         magic, version, devid);

  if (magic != VT_MAGIC)
    {
      printf("[vserial] bad magic\n");
      return false;
    }
  if (devid != VT_DEV_SERIAL)
    {
      printf("[vserial] not a serial device (id=%u)\n", devid);
      return false;
    }

  /* reset */
  reg_w(VTMMIO_STATUS, 0);
  __sync_synchronize();

  return (version == 2) ? init_v2() : init_v1();
}

/* ------------------------------------------------------------------ */
/* Vcon_svr implementation: write and read                            */
/* ------------------------------------------------------------------ */

void Virtio_serial_svr::vcon_write(const char *buf, unsigned size) noexcept
{
  if (!_dma || size == 0)
    return;
  if (size > TX_BUF_SIZE)
    size = TX_BUF_SIZE;

  memcpy(_dma->tx_buf, buf, size);
  __sync_synchronize();

  l4_uint16_t slot    = _tx_avail & (VQ_SIZE - 1);
  l4_uint64_t buf_pa  = _phys + __builtin_offsetof(uart_dma, tx_buf);

  _dma->txq.desc[slot].addr  = buf_pa;
  _dma->txq.desc[slot].len   = size;
  _dma->txq.desc[slot].flags = 0;
  _dma->txq.desc[slot].next  = 0;

  _dma->txq.avail.ring[_tx_avail & (VQ_SIZE - 1)] = slot;
  __sync_synchronize();
  _dma->txq.avail.idx = (l4_uint16_t)(_tx_avail + 1);
  __sync_synchronize();
  reg_w(VTMMIO_QUEUE_NOTIFY, 1);

  /* Poll for TX completion (non-blocking is also acceptable for serial) */
  for (unsigned i = 0; i < 100000 && _dma->txq.used.idx == _tx_last; ++i)
    ;

  reg_w(VTMMIO_IRQ_ACK, reg_r(VTMMIO_IRQ_STATUS));
  while (_tx_last != _dma->txq.used.idx)
    ++_tx_last;
  ++_tx_avail;
}

unsigned Virtio_serial_svr::vcon_read(char *buf, unsigned size) noexcept
{
  if (!_dma || _dma->rxq.used.idx == _rx_last)
    return 0;

  reg_w(VTMMIO_IRQ_ACK, reg_r(VTMMIO_IRQ_STATUS));

  unsigned total = 0;
  while (_rx_last != _dma->rxq.used.idx && total < size)
    {
      vq_used_elem &ue  = _dma->rxq.used.ring[_rx_last & (VQ_SIZE - 1)];
      l4_uint16_t   id  = (l4_uint16_t)ue.id;
      unsigned      len = ue.len;
      if (len > RX_BUF_SIZE) len = RX_BUF_SIZE;
      if (len > size - total) len = size - total;

      memcpy(buf + total, _dma->rx_buf[id], len);
      total += len;

      /* Re-arm the RX descriptor */
      l4_uint64_t base = _phys + __builtin_offsetof(uart_dma, rx_buf);
      _dma->rxq.desc[id].addr  = base + id * RX_BUF_SIZE;
      _dma->rxq.desc[id].len   = RX_BUF_SIZE;
      _dma->rxq.desc[id].flags = VTDESC_F_WRITE;
      _dma->rxq.desc[id].next  = 0;
      __sync_synchronize();
      _dma->rxq.avail.ring[_dma->rxq.avail.idx & (VQ_SIZE - 1)] = id;
      _dma->rxq.avail.idx = (l4_uint16_t)(_dma->rxq.avail.idx + 1);
      __sync_synchronize();
      reg_w(VTMMIO_QUEUE_NOTIFY, 0);

      ++_rx_last;
    }
  return total;
}

/* ------------------------------------------------------------------ */
/* Hardware scan via vbus                                             */
/* ------------------------------------------------------------------ */

static bool alloc_dma(uart_dma **dma_out, l4_uint64_t *phys_out)
{
  constexpr l4_size_t SZ =
    (sizeof(uart_dma) + L4_PAGESIZE - 1) & ~(l4_size_t)(L4_PAGESIZE - 1);

  auto dma_space = L4Re::chkcap(
    L4Re::Util::make_unique_cap<L4Re::Dma_space>(),
    "alloc dma_space cap");
  L4Re::chksys(
    L4Re::Env::env()->user_factory()->create(dma_space.get()),
    "create dma_space");
  L4Re::chksys(
    dma_space->associate(
      L4::Ipc::Cap<L4::Task>(),
      L4Re::Dma_space::Space_attrib::Phys_space),
    "associate dma phys");

  auto ds = L4Re::chkcap(
    L4Re::Util::make_unique_cap<L4Re::Dataspace>(),
    "alloc ds cap");
  L4Re::chksys(
    L4Re::Env::env()->mem_alloc()->alloc(
      SZ, ds.get(),
      L4Re::Mem_alloc::Continuous | L4Re::Mem_alloc::Pinned),
    "alloc dma mem");

  l4_addr_t va = 0;
  L4Re::chksys(
    L4Re::Env::env()->rm()->attach(
      &va, SZ,
      L4Re::Rm::F::Search_addr | L4Re::Rm::F::Eager_map | L4Re::Rm::F::RW,
      L4::Ipc::make_cap_rw(ds.get())),
    "attach dma mem");
  memset(reinterpret_cast<void *>(va), 0, SZ);

  l4_size_t mapped = SZ;
  L4Re::Dma_space::Dma_addr da = 0;
  L4Re::chksys(
    dma_space->map(
      L4::Ipc::make_cap(ds.get(), L4_CAP_FPAGE_RW),
      0, &mapped,
      L4Re::Dma_space::Attributes::None,
      L4Re::Dma_space::Direction::Bidirectional, &da),
    "dma map");

  (void)ds.release();
  (void)dma_space.release();

  *dma_out  = reinterpret_cast<uart_dma *>(va);
  *phys_out = (l4_uint64_t)da;
  return true;
}

static bool find_and_init_serial(L4::Cap<L4vbus::Vbus> vbus,
                                 Virtio_serial_svr *svr)
{
  L4vbus::Device dev;
  l4vbus_device_t di;

  while (vbus->root().next_device(&dev, L4VBUS_MAX_DEPTH, &di) == L4_EOK)
    {
      if (dev.is_compatible("virtio,mmio") != 1)
        continue;

      /* Collect MMIO resource */
      l4_addr_t  mmio_start = 0;
      l4_size_t  mmio_size  = 0;
      bool found_mmio = false;

      for (unsigned i = 0; i < di.num_resources; ++i)
        {
          l4vbus_resource_t res;
          if (dev.get_resource(i, &res) != L4_EOK) continue;
          if (res.type == L4VBUS_RESOURCE_MEM && !found_mmio)
            {
              mmio_start = res.start;
              mmio_size  = res.end - res.start + 1;
              found_mmio = true;
            }
        }

      if (!found_mmio)
        continue;

      /* Map MMIO uncached */
      l4_addr_t page_off  = mmio_start & (L4_PAGESIZE - 1);
      auto iods = L4::Ipc::make_cap_rw(
        L4::cap_reinterpret_cast<L4Re::Dataspace>(vbus));
      l4_addr_t mmio_virt = 0;
      if (L4Re::Env::env()->rm()->attach(
            &mmio_virt, mmio_size + page_off,
            L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW
            | L4Re::Rm::F::Cache_uncached,
            iods, mmio_start - page_off, L4_PAGESHIFT) < 0)
        continue;

      l4_addr_t dev_base  = mmio_virt + page_off;
      l4_uint32_t devid   =
        reinterpret_cast<volatile l4_uint32_t *>(dev_base)[2]; /* DeviceID */

      if (devid != VT_DEV_SERIAL)
        {
          L4Re::Env::env()->rm()->detach(mmio_virt, nullptr);
          continue;
        }

      printf("[vserial] found serial device at MMIO 0x%lx\n", mmio_start);

      uart_dma   *dma  = nullptr;
      l4_uint64_t phys = 0;
      if (!alloc_dma(&dma, &phys))
        {
          L4Re::Env::env()->rm()->detach(mmio_virt, nullptr);
          return false;
        }

      return svr->init(dev_base, phys, dma);
    }

  printf("[vserial] no virtio-serial device found on vbus\n");
  return false;
}

/* ------------------------------------------------------------------ */
/* Server loop                                                         */
/* ------------------------------------------------------------------ */

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> g_server;

int main()
{
  printf("[vserial] starting virtio-serial Vcon server\n");

  auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (!vbus.is_valid())
    {
      printf("[vserial] 'vbus' capability not found\n");
      return 1;
    }

  static Virtio_serial_svr svr;

  if (!find_and_init_serial(vbus, &svr))
    {
      printf("[vserial] hardware init failed\n");
      return 1;
    }

  auto cap = g_server.registry()->register_obj(&svr, "vcon");
  if (!cap.is_valid())
    {
      printf("[vserial] failed to register 'vcon' capability\n");
      return 1;
    }

  printf("[vserial] registered as 'vcon', entering server loop\n");
  g_server.loop();
  return 0;
}
