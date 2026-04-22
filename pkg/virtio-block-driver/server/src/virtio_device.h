/*
 * VirtIO MMIO block device driver for TuringOS
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Implements the VirtIO 1.x MMIO transport for block devices
 * as exposed by QEMU virt platform.
 */
#pragma once

#include <l4/re/util/shared_cap>
#include <l4/re/util/unique_cap>
#include <l4/re/dma_space>
#include <l4/re/dataspace>
#include <l4/re/util/object_registry>
#include <l4/sys/icu>
#include <l4/sys/irq>
#include <l4/cxx/ref_ptr>
#include <l4/l4virtio/virtio_block.h>
#include <l4/l4virtio/virtqueue>
#include <l4/libblock-device/device.h>
#include <l4/libblock-device/errand.h>
#include <l4/libblock-device/part_device.h>

#include "debug.h"

namespace Virtio {

// Number of descriptors in the virtqueue (must be power of 2)
enum : unsigned { Queue_size = 16 };

// VirtIO MMIO register offsets.
// QEMU 4.x uses VirtIO MMIO legacy (version 1); this driver targets that.
// Version 2 registers (QueueReady, QueueDesc*, etc.) are listed for reference
// but are not used.
enum : unsigned {
  // Common registers (v1 and v2)
  Mmio_magic           = 0x000,  // r: must read 0x74726976 ("virt")
  Mmio_version         = 0x004,  // r: 1=legacy, 2=modern
  Mmio_device_id       = 0x008,  // r: 2 = block device
  Mmio_vendor_id       = 0x00c,
  Mmio_dev_features    = 0x010,  // r: device feature bits
  Mmio_drv_features    = 0x020,  // w: guest/driver feature bits (GuestFeatures in v1)
  Mmio_queue_sel       = 0x030,  // w: queue index to configure
  Mmio_queue_num_max   = 0x034,  // r: max queue size
  Mmio_queue_num       = 0x038,  // w: queue size to use
  Mmio_queue_notify    = 0x050,  // w: notify device of new work
  Mmio_int_status      = 0x060,  // r: interrupt reason bits
  Mmio_int_ack         = 0x064,  // w: clear interrupt bits
  Mmio_status          = 0x070,  // rw: device status

  // Version 1 (legacy) only
  Mmio_guest_page_size = 0x028,  // w: guest OS page size (must set before queue setup)
  Mmio_queue_align     = 0x03c,  // w: virtqueue section alignment
  Mmio_queue_pfn       = 0x040,  // w: physical page number of virtqueue base

  // Config space
  Mmio_config          = 0x100,  // device-specific config
};

// VirtIO interrupt status bits
enum : l4_uint32_t {
  Int_used_ring = 0x1,  // used ring updated
  Int_config    = 0x2,  // config space changed
};

// VirtIO descriptor flags
enum : l4_uint16_t {
  Desc_next     = 0x1,  // descriptor chains to next
  Desc_write    = 0x2,  // device writes to this buffer
  Desc_indirect = 0x4,  // buffer contains indirect table
};

// VirtIO MMIO magic value
enum : l4_uint32_t { Virtio_magic = 0x74726976 };

// Virtqueue descriptor (16 bytes, spec section 2.7.5)
struct Vq_desc
{
  l4_uint64_t addr;   // physical/DMA address of buffer
  l4_uint32_t len;    // length of buffer
  l4_uint16_t flags;  // Desc_* flags
  l4_uint16_t next;   // index of next descriptor (if Desc_next set)
} __attribute__((packed));

static_assert(sizeof(Vq_desc) == 16, "Vq_desc must be 16 bytes");

// Available ring (driver → device)
struct Vq_avail
{
  l4_uint16_t flags;
  l4_uint16_t idx;
  l4_uint16_t ring[Queue_size];
  l4_uint16_t used_event;  // optional: suppress used ring notifications
} __attribute__((packed));

// Used ring element (device → driver)
struct Vq_used_elem
{
  l4_uint32_t id;   // head descriptor index of completed request
  l4_uint32_t len;  // bytes written by device (for reads)
} __attribute__((packed));

// Used ring (device → driver)
struct Vq_used
{
  l4_uint16_t   flags;
  l4_uint16_t   idx;
  Vq_used_elem  ring[Queue_size];
  l4_uint16_t   avail_event;  // optional: suppress avail ring notifications
} __attribute__((packed));

// Virtqueue memory layout for VirtIO MMIO v1 (legacy) with QueueAlign = L4_PAGESIZE.
//
// QEMU v1 places the used ring at:
//   round_up(desc_size + avail_size, QueueAlign)
//   = round_up(256 + 36, 4096) = 4096
//
// So we allocate TWO physically-contiguous 4KB pages:
//   Page 0: descriptor table + available ring
//   Page 1: used ring + block-request headers + per-request status bytes
//
// QueuePFN = _vq_phys >> L4_PAGESHIFT  (physical base of page 0)
enum : l4_size_t {
  Vq_desc_off    = 0,      // Queue_size * 16  = 256 bytes (at page 0 base)
  Vq_avail_off   = 256,    // 4 + Queue_size*2 =  36 bytes (no used_event in v1)
  Vq_used_off    = 4096,   // at next page boundary (QueueAlign = 4096)
  Vq_hdr_off     = 4096 + 4 + Queue_size * 8,  // after used ring (= 4228)
  Vq_status_off  = 4228 + Queue_size * 16,      // after request headers (= 4484)
  Vq_mem_size    = 8192,   // two pages
};

// End-of-queue sentinel for free list
enum : l4_uint16_t { Eoq = 0xffffu };


/**
 * VirtIO MMIO block device.
 *
 * Directly accesses VirtIO MMIO registers (mapped from vbus) and implements
 * the Block_device::Device interface for use with libblock-device.
 */
class Block_dev
: public Block_device::Device_with_notification_domain<Block_device::Device>,
  public L4::Irqep_t<Block_dev>
{
public:
  /**
   * Construct from an already-mapped MMIO region.
   *
   * \param mmio_virt  Local virtual address of the VirtIO MMIO registers.
   * \param dma        DMA space bound to this device's bus.
   * \param index      Device index for match_hid (e.g. 0 → "virtio-blk0").
   */
  Block_dev(l4_addr_t mmio_virt,
            L4Re::Util::Shared_cap<L4Re::Dma_space> dma,
            unsigned index);
  ~Block_dev();

  // ----- Block_device::Device interface -----

  bool is_read_only() const override { return _read_only; }
  bool match_hid(cxx::String const &hid) const override;
  l4_uint64_t capacity() const override { return _capacity; }
  l4_size_t sector_size() const override { return 512; }
  l4_size_t max_size() const override
  { return (Queue_size - 2) * 512; }  // max per single scatter segment
  unsigned max_segments() const override { return Queue_size - 2; }

  void reset() override;

  int dma_map(Block_device::Mem_region *region, l4_addr_t offset,
              l4_size_t num_sectors, L4Re::Dma_space::Direction dir,
              L4Re::Dma_space::Dma_addr *phys) override;

  int dma_unmap(L4Re::Dma_space::Dma_addr phys, l4_size_t num_sectors,
                L4Re::Dma_space::Direction dir) override;

  int inout_data(l4_uint64_t sector, Block_device::Inout_block const &blocks,
                 Block_device::Inout_callback const &cb,
                 L4Re::Dma_space::Direction dir) override;

  int flush(Block_device::Inout_callback const &cb) override;

  void start_device_scan(Block_device::Errand::Callback const &callback) override;

  // ----- IRQ handler (called by L4::Irqep_t dispatch) -----
  void handle_irq();

  // ----- Setup helpers called from main -----

  /** Initialize VirtIO device and set up the virtqueue. */
  void init();

  /**
   * Register IRQ handler with vbus ICU.
   *
   * \param icu           ICU capability from vbus.
   * \param registry      Object registry (Errand_server's registry).
   * \param irq_num       Hardware IRQ number (from vbus resource).
   * \param edge          True for edge-triggered, false for level.
   */
  void register_irq(L4::Cap<L4::Icu> icu,
                    L4::Registry_iface *registry,
                    unsigned irq_num, bool edge);

private:
  // Register I/O helpers
  l4_uint32_t reg_read(unsigned off) const
  {
    return reinterpret_cast<volatile l4_uint32_t *>(_mmio_base)[off / 4];
  }
  void reg_write(unsigned off, l4_uint32_t val)
  {
    reinterpret_cast<volatile l4_uint32_t *>(_mmio_base)[off / 4] = val;
  }

  // Descriptor free-list management
  int  alloc_desc();
  void free_chain(unsigned head);

  // Virtqueue submission and completion
  void submit_avail(unsigned head);
  void process_used();

  // Helper: build and submit one request
  int submit_request(l4_uint32_t type, l4_uint64_t sector,
                     Block_device::Inout_block const *blocks,
                     Block_device::Inout_callback const &cb,
                     L4Re::Dma_space::Direction dir);

  // MMIO base virtual address
  l4_addr_t _mmio_base;

  // DMA space for mapping client and internal buffers
  L4Re::Util::Shared_cap<L4Re::Dma_space> _dma;

  // Device index (used in match_hid)
  unsigned _index;

  // Properties read from device config
  l4_uint64_t _capacity = 0;  // device capacity in bytes
  bool _read_only = false;
  bool _edge_triggered = true;

  // Virtqueue memory: one physically-contiguous pinned page
  L4Re::Util::Unique_cap<L4Re::Dataspace> _vq_ds;
  l4_addr_t                       _vq_virt = 0;  // local virtual address
  L4Re::Dma_space::Dma_addr       _vq_phys = 0;  // DMA address seen by device

  // Virtqueue structure pointers (into _vq_virt)
  Vq_desc                  *_desc   = nullptr;
  Vq_avail                 *_avail  = nullptr;
  Vq_used                  *_used   = nullptr;
  l4virtio_block_header_t  *_hdrs   = nullptr;  // one per queue slot
  l4_uint8_t               *_status = nullptr;  // one byte per queue slot

  // Descriptor free-list (singly-linked via Vq_desc::next)
  l4_uint16_t _free_head = 0;
  unsigned    _num_free  = Queue_size;

  // Virtqueue shadow indices
  l4_uint16_t _avail_idx = 0;  // next slot to write in avail->ring
  l4_uint16_t _used_idx  = 0;  // last used->idx we processed

  // Per-request pending state, indexed by head descriptor index
  struct Pending
  {
    Block_device::Inout_callback cb;
    l4_size_t                   size  = 0;
    bool                        valid = false;
  };
  Pending _pending[Queue_size];
};

} // namespace Virtio
