/*
 * VirtIO Block Device Implementation
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 */

#include "virtio_device.h"
#include "debug.h"
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/re/dma_space>
#include <string.h>

namespace Virtio {

Block_device::Block_device(L4::cap_idx_t device_cap)
: _device(L4::cap_cast<L4virtio::Device>(device_cap))
{
}

Block_device::~Block_device()
{
  if (_queue_data)
    free(_queue_data);
}

cxx::unique_ptr<Block_device> Block_device::create()
{
  // Try to find VirtIO block device in environment
  auto *env = L4Re::Env::env();

  // Look for VirtIO device capability
  // This requires proper device tree parsing or environment configuration
  // For now, we'll assume the device is accessible via a known path

  L4::Cap<L4virtio::Device> dev_cap;

  // In a real implementation, we would:
  // 1. Parse device tree for virtio-mmio devices
  // 2. Find the virtio-block device
  // 3. Initialize it

  // For testing/development, we might need to pass the device capability
  // via command line or environment variable

  if (!dev_cap.is_valid())
    {
      Dbg::warn().printf("No VirtIO block device found in environment\n");
      return nullptr;
    }

  auto device = cxx::make_unique<Block_device>(dev_cap.idx());

  // Initialize the device
  device->init_virtio_device();

  return device;
}

void Block_device::init_virtio_device()
{
  Dbg::info().printf("Initializing VirtIO block device...\n");

  // Reset the device
  _device->set_status(0);

  // Acknowledge device and wait for driver
  unsigned status = L4VIRTIO_STATUS_ACKNOWLEDGE | L4VIRTIO_STATUS_DRIVER;
  _device->set_status(status);

  // Read device features
  _device->get_device_features(0, &_features);
  Dbg::trace().printf("Device features: 0x%llx\n", (unsigned long long)_features);

  // Read device configuration
  L4Re::chksys(_device->device_config(_config_ds, nullptr),
               "Reading VirtIO block device configuration");

  // Map configuration datumspace and read config
  // In real implementation, we need to properly handle the config dataspace

  // For now, use default values for QEMU virtio-blk
  // 100MB = 204800 sectors
  _capacity = 204800ULL * 512ULL;
  _max_req_size = 64 * 1024;  // 64KB max request

  Dbg::info().printf("Device capacity: %llu bytes (%llu MB)\n",
                     (unsigned long long)_capacity,
                     (unsigned long long)(_capacity / (1024 * 1024)));

  // Configure request queues
  int ret = configure_queues();
  if (ret < 0)
    {
      Dbg::error().printf("Failed to configure queues: %d\n", ret);
      return;
    }

  // Set driver OK
  status |= L4VIRTIO_STATUS_DRIVER_OK;
  _device->set_status(status);

  Dbg::info().printf("VirtIO block device initialized\n");
}

int Block_device::configure_queues()
{
  // VirtIO block device has one request queue (queue 0)
  unsigned queue = 0;

  // Get queue size
  l4virtio_queue_info_t queue_info;
  L4Re::chksys(_device->config_queue(queue, &queue_info),
               "Getting VirtIO queue configuration");

  _queue_size = queue_info.num;
  Dbg::trace().printf("Queue %u size: %u\n", queue, _queue_size);

  // Allocate queue data area
  size_t queue_data_size = l4virtio_queue_size(_queue_size);
  _queue_data = malloc(queue_data_size);
  if (!_queue_data)
    {
      Dbg::error().printf("Failed to allocate queue data\n");
      return -L4_ENOMEM;
    }

  // Set up queue data
  // This would normally involve registering the datumspace with the device
  // and setting up descriptor chains

  Dbg::trace().printf("Queue configured\n");
  return 0;
}

void Block_device::start_device_scan(Block_device::Errand::Callback const &callback)
{
  Dbg::trace().printf("Starting device scan\n");

  // For VirtIO block device, we don't need to scan for physical devices
  // The device is already discovered during initialization

  callback();
}

void Block_device::reset()
{
  Dbg::trace().printf("Resetting device\n");

  // Reset VirtIO device
  _device->set_status(0);
}

bool Block_device::match_hid(cxx::String const &hid) const
{
  // Match against device ID or other identifiers
  if (_device_id.empty())
    return false;

  return std::string(hid.start(), hid.length()) == std::string(_device_id.begin(), _device_id.end());
}

int Block_device::dma_map(Block_device::Mem_region *region, l4_addr_t offset,
                          l4_size_t num_sectors, L4Re::Dma_space::Direction dir,
                          L4Re::Dma_space::Dma_addr *phys)
{
  // For VirtIO devices, DMA is typically handled by the device itself
  // We may need to handle this differently depending on the platform

  // For now, return a placeholder physical address
  *phys = offset;
  return L4_EOK;
}

int Block_device::dma_unmap(L4Re::Dma_space::Dma_addr phys, l4_size_t num_sectors,
                            L4Re::Dma_space::Direction dir)
{
  // Clean up DMA mapping
  return L4_EOK;
}

int Block_device::inout_data(l4_uint64_t sector, Block_device::Inout_block const &blocks,
                             Block_device::Inout_callback const &cb,
                             L4Re::Dma_space::Direction dir)
{
  Dbg::trace().printf("inout_data: sector=%llu dir=%d\n",
                     (unsigned long long)sector, static_cast<int>(dir));

  // Process the block list
  l4_uint64_t current_sector = sector;
  const Block_device::Inout_block *current_block = &blocks;
  l4_size_t total_bytes = 0;

  while (current_block)
    {
      l4_uint32_t type = (dir == L4Re::Dma_space::From_device) ?
                         L4VIRTIO_BLOCK_T_IN : L4VIRTIO_BLOCK_T_OUT;

      int ret = submit_virtio_request(type, current_sector,
                                      current_block->data,
                                      current_block->size / 512,
                                      cb);

      if (ret < 0)
        return ret;

      current_sector += (current_block->size / 512);
      total_bytes += current_block->size;
      current_block = current_block->next;
    }

  // Call completion callback if provided
  if (cb)
    cb(L4_EOK);

  return L4_EOK;
}

int Block_device::flush(Block_device::Inout_callback const &cb)
{
  Dbg::trace().printf("Flush\n");

  // VirtIO block flush operation
  int ret = submit_virtio_request(L4VIRTIO_BLOCK_T_FLUSH, 0, nullptr, 0, cb);

  // Call completion callback if provided
  if (cb && ret >= 0)
    cb(L4_EOK);

  return ret;
}

int Block_device::submit_virtio_request(l4_uint32_t type, l4_uint64_t sector,
                                        void *buffer, l4_size_t count,
                                        Block_device::Inout_callback const &cb)
{
  // Build VirtIO block request
  l4virtio_block_header_t header;
  header.type = type;
  header.ioprio = 0;
  header.sector = sector;

  l4virtio_block_status_u status_val;
  status_val.raw = 0;

  // In a real implementation, this would:
  // 1. Allocate virtqueue descriptors
  // 2. Set up the descriptor chain: [header] [data buffer] [status]
  // 3. Notify the device
  // 4. Wait for completion
  // 5. Check status

  // For now, return success for testing
  Dbg::trace().printf("VirtIO request: type=%u sector=%llu count=%zu (stub)\n",
                     type, (unsigned long long)sector, count);

  return 0;
}

void Block_device::handle_interrupt()
{
  Dbg::trace().printf("Interrupt received\n");

  // Handle VirtIO device interrupts
  // Process completed requests and notify waiting errands
}

int Partitioned_device::inout_data(l4_uint64_t sector, Block_device::Inout_block const &blocks,
                                   Block_device::Inout_callback const &cb,
                                   L4Re::Dma_space::Direction dir)
{
  // Convert partition-relative sector to device-absolute sector
  l4_uint64_t abs_sector = _info.start_sector + sector;
  return _parent->inout_data(abs_sector, blocks, cb, dir);
}

bool Partitioned_device::match_hid(cxx::String const &hid) const
{
  // Check if HID matches parent device or this specific partition
  if (_parent->match_hid(hid))
    return true;

  // Check for partition-specific format: "device:partition_number"
  const char *colon = static_cast<const char*>(memchr(hid.start(), ':', hid.length()));
  if (colon)
    {
      cxx::String parent_hid(hid.start(), colon - hid.start());

      if (_parent->match_hid(parent_hid))
        {
          // Check partition number
          const char *part_str = colon + 1;
          cxx::String part_num(part_str, hid.length() - (colon - hid.start()) - 1);

          // Convert to integer and compare
          unsigned requested_part = 0;
          for (size_t i = 0; i < part_num.length(); ++i)
            {
              char c = part_num[i];
              if ('0' <= c && c <= '9')
                {
                  requested_part = requested_part * 10 + (c - '0');
                }
              else
                {
                  return false;
                }
            }

          return requested_part == _partition_id;
        }
    }

  return false;
}

} // namespace Virtio