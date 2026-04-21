/*
 * VirtIO Block Device Implementation
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 */

#ifndef VIRTIO_DEVICE_H
#define VIRTIO_DEVICE_H

#include <l4/l4virtio/l4virtio>
#include <l4/l4virtio/virtio_block.h>
#include <l4/libblock-device/device.h>

namespace Virtio {

/**
 * VirtIO Block Device implementation
 */
class Block_device : public Block_device::Device_with_notification_domain<Block_device::Device>
{
public:
  using Device_type = Block_device;

  Block_device(L4::cap_idx_t device_cap);
  virtual ~Block_device();

  /**
   * Create a VirtIO block device from the environment
   */
  static cxx::unique_ptr<Block_device> create();

  // Block_device interface
  bool is_read_only() const override { return _read_only; }
  bool match_hid(cxx::String const &hid) const override;
  l4_uint64_t capacity() const override { return _capacity; }
  l4_size_t sector_size() const override { return 512; }
  l4_size_t max_size() const override { return _max_req_size; }
  unsigned max_segments() const override { return 16; }

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

  // Device identification
  const char *name() const { return "virtio-blk"; }

private:
  /**
   * Initialize VirtIO device
   */
  void init_virtio_device();

  /**
   * Configure VirtIO block request queue
   */
  int configure_queues();

  /**
   * Handle VirtIO device interrupts
   */
  void handle_interrupt();

  /**
   * Submit a block request to the VirtIO device
   */
  int submit_virtio_request(l4_uint32_t type, l4_uint64_t sector,
                           void *buffer, l4_size_t count,
                           Block_device::Inout_callback const &cb);

  L4::Cap<L4virtio::Device> _device;
  L4::Cap<L4Re::Dataspace> _config_ds;

  // Device capabilities
  l4_uint64_t _capacity = 0;      // in bytes
  l4_size_t _max_req_size = 0;    // maximum request size in bytes
  bool _read_only = false;

  // VirtIO specific
  std::vector<char> _device_id;
  l4_uint64_t _features = 0;
  l4_uint32_t _queue_size = 0;
  void *_queue_data = nullptr;
};

/**
 * Partitioned VirtIO Block Device
 */
class Partitioned_device : public Block_device::Device_with_notification_domain<Block_device::Device>
{
public:
  Partitioned_device(cxx::Ref_ptr<Block_device> const &parent,
                     unsigned partition_id,
                     Block_device::Partition_info const &info)
  : _parent(parent), _partition_id(partition_id), _info(info)
  {}

  bool is_read_only() const override { return _parent->is_read_only(); }
  bool match_hid(cxx::String const &hid) const override;
  l4_uint64_t capacity() const override { return _info.sectors * 512; }
  l4_size_t sector_size() const override { return _parent->sector_size(); }
  l4_size_t max_size() const override { return _parent->max_size(); }
  unsigned max_segments() const override { return _parent->max_segments(); }

  void reset() override { _parent->reset(); }
  int dma_map(Block_device::Mem_region *region, l4_addr_t offset,
              l4_size_t num_sectors, L4Re::Dma_space::Direction dir,
              L4Re::Dma_space::Dma_addr *phys) override
  { return _parent->dma_map(region, offset, num_sectors, dir, phys); }

  int dma_unmap(L4Re::Dma_space::Dma_addr phys, l4_size_t num_sectors,
                L4Re::Dma_space::Direction dir) override
  { return _parent->dma_unmap(phys, num_sectors, dir); }

  int inout_data(l4_uint64_t sector, Block_device::Inout_block const &blocks,
                 Block_device::Inout_callback const &cb,
                 L4Re::Dma_space::Direction dir) override;

  int flush(Block_device::Inout_callback const &cb) override
  { return _parent->flush(cb); }

  void start_device_scan(Block_device::Errand::Callback const &callback) override
  { _parent->start_device_scan(callback); }

  const char *name() const { return _parent->name(); }

private:
  cxx::Ref_ptr<Block_device> _parent;
  unsigned _partition_id;
  Block_device::Partition_info _info;
};

} // namespace Virtio

#endif // VIRTIO_DEVICE_H