/*
 * VirtIO MMIO block device implementation
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */

#include "virtio_device.h"
#include "debug.h"

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/re/mem_alloc>
#include <l4/re/rm>
#include <l4/sys/err.h>
#include <l4/l4virtio/virtqueue>  // L4virtio::wmb()/rmb()

#include <cstring>
#include <cstdio>

namespace Virtio {

// -----------------------------------------------------------------------
// Constructor / Destructor
// -----------------------------------------------------------------------

Block_dev::Block_dev(l4_addr_t mmio_virt,
                     L4Re::Util::Shared_cap<L4Re::Dma_space> dma,
                     unsigned index)
: _mmio_base(mmio_virt), _dma(dma), _index(index)
{}

Block_dev::~Block_dev()
{
  // Unmap virtqueue memory if it was mapped
  if (_vq_virt)
    L4Re::Env::env()->rm()->detach(_vq_virt, nullptr);
}

// -----------------------------------------------------------------------
// Block_device::Device interface
// -----------------------------------------------------------------------

bool Block_dev::match_hid(cxx::String const &hid) const
{
  char buf[24];
  int n = snprintf(buf, sizeof(buf), "virtio-blk%u", _index);
  return hid == cxx::String(buf, n);
}

void Block_dev::reset()
{
  Dbg::trace().printf("Resetting VirtIO device\n");
  reg_write(Mmio_status, 0);
  // Caller (virtio_client) will call init() again if needed; we just reset
  // the hardware.  For now we keep our software state as-is since the
  // libblock-device framework will re-initialise the client.
}

int Block_dev::dma_map(Block_device::Mem_region *region, l4_addr_t offset,
                       l4_size_t num_sectors, L4Re::Dma_space::Direction dir,
                       L4Re::Dma_space::Dma_addr *phys)
{
  l4_size_t size = num_sectors * sector_size();
  return _dma->map(L4::Ipc::make_cap_rw(region->ds()),
                   region->ds_offset() + offset,
                   &size,
                   L4Re::Dma_space::Attributes::None,
                   dir, phys);
}

int Block_dev::dma_unmap(L4Re::Dma_space::Dma_addr phys, l4_size_t num_sectors,
                         L4Re::Dma_space::Direction dir)
{
  return _dma->unmap(phys, num_sectors * sector_size(),
                     L4Re::Dma_space::Attributes::None, dir);
}

int Block_dev::inout_data(l4_uint64_t sector,
                          Block_device::Inout_block const &blocks,
                          Block_device::Inout_callback const &cb,
                          L4Re::Dma_space::Direction dir)
{
  l4_uint32_t type = (dir == L4Re::Dma_space::Direction::To_device)
                     ? L4VIRTIO_BLOCK_T_OUT
                     : L4VIRTIO_BLOCK_T_IN;

  return submit_request(type, sector, &blocks, cb, dir);
}

int Block_dev::flush(Block_device::Inout_callback const &cb)
{
  return submit_request(L4VIRTIO_BLOCK_T_FLUSH, 0, nullptr, cb,
                        L4Re::Dma_space::Direction::None);
}

void Block_dev::start_device_scan(Block_device::Errand::Callback const &callback)
{
  // VirtIO block is already initialised by the time add_disk() is called
  callback();
}

// -----------------------------------------------------------------------
// IRQ handler
// -----------------------------------------------------------------------

void Block_dev::handle_irq()
{
  l4_uint32_t st = reg_read(Mmio_int_status);

  if (st & Int_used_ring)
    process_used();

  // Acknowledge the interrupt to the device
  reg_write(Mmio_int_ack, st);

  // Re-arm for the next event (needed for both edge and level)
  obj_cap()->unmask();
}

// -----------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------

void Block_dev::init()
{
  Dbg::info().printf("Initialising VirtIO MMIO block device (index %u)\n",
                     _index);

  // --- Sanity checks ---
  l4_uint32_t magic = reg_read(Mmio_magic);
  if (magic != Virtio_magic)
    {
      Dbg::warn().printf("Bad VirtIO magic 0x%08x\n", magic);
      L4Re::chksys(-L4_EINVAL, "Bad VirtIO magic");
    }

  l4_uint32_t ver = reg_read(Mmio_version);
  if (ver != 1)
    {
      Dbg::warn().printf("Unsupported VirtIO MMIO version %u (need 1/legacy)\n",
                         ver);
      L4Re::chksys(-L4_EINVAL, "Unsupported VirtIO version");
    }

  if (reg_read(Mmio_device_id) != 2)
    {
      Dbg::warn().printf("Not a block device (DeviceID=%u)\n",
                         reg_read(Mmio_device_id));
      L4Re::chksys(-L4_ENODEV, "Not a VirtIO block device");
    }

  // --- VirtIO MMIO v1 (legacy) initialisation sequence ---

  // Step 1: reset
  reg_write(Mmio_status, 0);

  // Step 2: ACKNOWLEDGE + DRIVER
  l4_uint32_t status = L4VIRTIO_STATUS_ACKNOWLEDGE | L4VIRTIO_STATUS_DRIVER;
  reg_write(Mmio_status, status);

  // Step 3: feature negotiation (v1 is 32-bit, no selector registers)
  l4_uint32_t features = reg_read(Mmio_dev_features);
  Dbg::info().printf("Device features: 0x%08x\n", features);
  reg_write(Mmio_drv_features, features);  // GuestFeatures

  // Step 4: set GuestPageSize (mandatory before queue setup in v1)
  reg_write(Mmio_guest_page_size, L4_PAGESIZE);

  // Step 5: read device config (capacity in 512-byte sectors at config+0/+4)
  l4_uint32_t cap_lo = reg_read(Mmio_config + 0);
  l4_uint32_t cap_hi = reg_read(Mmio_config + 4);
  l4_uint64_t sectors = ((l4_uint64_t)cap_hi << 32) | cap_lo;
  _capacity   = sectors * 512;
  _read_only  = !!(features & (1u << 5));  // VIRTIO_BLK_F_RO

  Dbg::info().printf("Block device: %llu sectors (%llu MiB)%s\n",
                     (unsigned long long)sectors,
                     (unsigned long long)(_capacity >> 20),
                     _read_only ? " [read-only]" : "");

  // Step 6: queue setup
  reg_write(Mmio_queue_sel, 0);
  l4_uint32_t qmax = reg_read(Mmio_queue_num_max);
  if (qmax < Queue_size)
    {
      Dbg::warn().printf("Device queue max %u < needed %u\n",
                         qmax, (unsigned)Queue_size);
      L4Re::chksys(-L4_EINVAL, "Queue too small");
    }
  reg_write(Mmio_queue_num, Queue_size);
  // QueueAlign controls where the used ring starts relative to the queue base.
  // With QueueAlign = L4_PAGESIZE, used ring starts at QueuePFN*pagesize + pagesize.
  reg_write(Mmio_queue_align, L4_PAGESIZE);

  // Allocate two physically-contiguous pages for the virtqueue
  _vq_ds = L4Re::chkcap(
    L4Re::Util::make_unique_cap<L4Re::Dataspace>(),
    "Alloc virtqueue dataspace cap");

  auto *env = L4Re::Env::env();
  L4Re::chksys(
    env->mem_alloc()->alloc(Vq_mem_size, _vq_ds.get(),
                            L4Re::Mem_alloc::Continuous
                            | L4Re::Mem_alloc::Pinned),
    "Alloc virtqueue memory");

  L4Re::chksys(
    env->rm()->attach(&_vq_virt, Vq_mem_size,
                      L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
                      L4::Ipc::make_cap_rw(_vq_ds.get()),
                      0, L4_PAGESHIFT),
    "Map virtqueue memory");

  memset(reinterpret_cast<void *>(_vq_virt), 0, Vq_mem_size);

  // Get the physical (DMA) address of the pinned virtqueue buffer.
  // The DMA space was associated with Phys_space (identity mapping, no IOMMU),
  // so map() returns the physical address directly.
  l4_size_t sz = Vq_mem_size;
  L4Re::chksys(
    _dma->map(L4::Ipc::make_cap_rw(_vq_ds.get()), 0, &sz,
              L4Re::Dma_space::Attributes::None,
              L4Re::Dma_space::Direction::Bidirectional,
              &_vq_phys),
    "Map virtqueue memory for DMA");

  Dbg::info().printf("Virtqueue at virt=0x%lx phys=0x%llx\n",
                     _vq_virt, (unsigned long long)_vq_phys);

  // v1: write the physical page number (all three ring parts start at this page)
  l4_uint32_t pfn = static_cast<l4_uint32_t>(_vq_phys >> L4_PAGESHIFT);
  reg_write(Mmio_queue_pfn, pfn);

  // Step 7: DRIVER_OK (no FEATURES_OK step in v1)
  status |= L4VIRTIO_STATUS_DRIVER_OK;
  reg_write(Mmio_status, status);

  // Set up virtqueue sub-structure pointers
  _desc   = reinterpret_cast<Vq_desc *>(_vq_virt + Vq_desc_off);
  _avail  = reinterpret_cast<Vq_avail *>(_vq_virt + Vq_avail_off);
  _used   = reinterpret_cast<Vq_used *>(_vq_virt + Vq_used_off);
  _hdrs   = reinterpret_cast<l4virtio_block_header_t *>(_vq_virt + Vq_hdr_off);
  _status = reinterpret_cast<l4_uint8_t *>(_vq_virt + Vq_status_off);

  // Initialise descriptor free-list: 0→1→…→(Queue_size-1)→Eoq
  for (unsigned i = 0; i < Queue_size - 1; ++i)
    _desc[i].next = static_cast<l4_uint16_t>(i + 1);
  _desc[Queue_size - 1].next = Eoq;
  _free_head = 0;
  _num_free  = Queue_size;
  _avail_idx = 0;
  _used_idx  = 0;

  Dbg::info().printf("VirtIO block device ready (pfn=0x%x qsize=%u)\n",
                     pfn, Queue_size);
}

void Block_dev::register_irq(L4::Cap<L4::Icu> icu,
                             L4::Registry_iface *registry,
                             unsigned irq_num, bool edge)
{
  _edge_triggered = edge;

  auto cap = L4Re::chkcap(registry->register_irq_obj(this),
                          "Register VirtIO IRQ object");

  L4Re::chksys(l4_error(icu->bind(irq_num, cap)),
               "Bind VirtIO IRQ to ICU");

  L4Re::chksys(l4_ipc_error(cap->unmask(), l4_utcb()),
               "Unmask VirtIO IRQ");

  Dbg::info().printf("Bound IRQ %u (edge=%d)\n", irq_num, (int)edge);
}

// -----------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------

int Block_dev::alloc_desc()
{
  if (_num_free == 0)
    return -1;
  int idx = _free_head;
  _free_head = _desc[idx].next;
  --_num_free;
  _desc[idx].flags = 0;
  _desc[idx].next  = Eoq;
  return idx;
}

void Block_dev::free_chain(unsigned head)
{
  unsigned idx = head;
  for (;;)
    {
      bool has_next = !!(_desc[idx].flags & Desc_next);
      unsigned next = _desc[idx].next;
      _desc[idx].flags = 0;
      _desc[idx].addr  = 0;
      _desc[idx].len   = 0;
      _desc[idx].next  = _free_head;
      _free_head = static_cast<l4_uint16_t>(idx);
      ++_num_free;
      if (!has_next)
        break;
      idx = next;
    }
  _pending[head].valid = false;
}

void Block_dev::submit_avail(unsigned head)
{
  // Write head to available ring
  _avail->ring[_avail_idx % Queue_size] = static_cast<l4_uint16_t>(head);

  L4virtio::wmb();  // ensure descriptor chain is visible before idx update

  ++_avail_idx;
  _avail->idx = _avail_idx;

  L4virtio::wmb();  // ensure idx update is visible before doorbell

  // Notify device: write queue index to QueueNotify
  reg_write(Mmio_queue_notify, 0);
}

void Block_dev::process_used()
{
  L4virtio::rmb();  // ensure used ring is visible

  while (_used_idx != _used->idx)
    {
      unsigned slot = _used_idx % Queue_size;
      l4_uint32_t head = _used->ring[slot].id;

      L4virtio::rmb();

      if (head < Queue_size && _pending[head].valid)
        {
          l4_uint8_t st   = _status[head];
          int        err  = (st == L4VIRTIO_BLOCK_S_OK) ? L4_EOK : -L4_EIO;
          l4_size_t  size = _pending[head].size;

          Block_device::Inout_callback cb = cxx::move(_pending[head].cb);
          free_chain(head);

          // Dispatch completion callback via errand (next server loop tick)
          Block_device::Errand::schedule(
            [cb, err, size]() { cb(err, size); }, 0);
        }
      else
        {
          Dbg::warn().printf("Unexpected used entry: head=%u valid=%d\n",
                             head,
                             head < Queue_size ? (int)_pending[head].valid : -1);
        }

      ++_used_idx;
    }
}

int Block_dev::submit_request(l4_uint32_t type, l4_uint64_t sector,
                              Block_device::Inout_block const *blocks,
                              Block_device::Inout_callback const &cb,
                              L4Re::Dma_space::Direction dir)
{
  // Count scatter-gather blocks
  unsigned nblocks = 0;
  for (auto *b = blocks; b; b = b->next.get())
    ++nblocks;

  // Need: 1 header + nblocks data + 1 status
  unsigned needed = nblocks + 2;
  if (_num_free < needed)
    {
      Dbg::trace().printf("Queue full (free=%u needed=%u)\n",
                          _num_free, needed);
      return -L4_EBUSY;
    }

  // Allocate head descriptor (will hold the request header)
  int head = alloc_desc();
  // Guaranteed to succeed because we checked _num_free above.

  // Initialise header in the pre-allocated header buffer
  _hdrs[head].type   = type;
  _hdrs[head].ioprio = 0;
  _hdrs[head].sector = sector;
  _status[head]      = 0xff;  // initialise status to "invalid"

  // Header descriptor
  _desc[head].addr  = _vq_phys + Vq_hdr_off
                      + head * sizeof(l4virtio_block_header_t);
  _desc[head].len   = sizeof(l4virtio_block_header_t);
  _desc[head].flags = Desc_next;

  int prev = head;
  l4_size_t total_bytes = 0;

  // Data descriptors
  for (auto *b = blocks; b; b = b->next.get())
    {
      int d = alloc_desc();
      _desc[prev].next = static_cast<l4_uint16_t>(d);
      prev = d;

      _desc[d].addr  = b->dma_addr;
      _desc[d].len   = b->num_sectors * 512;
      // For reads (From_device), the device writes into the buffer
      _desc[d].flags = static_cast<l4_uint16_t>(
        (dir == L4Re::Dma_space::Direction::From_device ? Desc_write : 0)
        | Desc_next);

      total_bytes += _desc[d].len;
    }

  // Status descriptor (device always writes the status byte)
  int tail = alloc_desc();
  _desc[prev].next  = static_cast<l4_uint16_t>(tail);
  _desc[prev].flags = static_cast<l4_uint16_t>(_desc[prev].flags | Desc_next);

  _desc[tail].addr  = _vq_phys + Vq_status_off + head;
  _desc[tail].len   = 1;
  _desc[tail].flags = Desc_write;  // device writes, no Desc_next

  // Record callback info
  _pending[head].cb    = cb;
  _pending[head].size  = total_bytes;
  _pending[head].valid = true;

  submit_avail(head);
  return L4_EOK;
}

} // namespace Virtio
