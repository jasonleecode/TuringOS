/*
 * VirtIO Block Device Driver for TuringOS
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 *
 * This driver implements support for VirtIO block devices in QEMU virt platform.
 */

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <getopt.h>
#include <map>
#include <vector>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/l4virtio/l4virtio>
#include <l4/l4virtio/virtio_block.h>

#include <terminate_handler-l4>

#include "virtio_device.h"
#include "debug.h"

#include <l4/libblock-device/block_device_mgr.h>
#include <l4/libblock-device/virtio_client.h>

static char const *const usage_str =
"Usage: %s [-vqd] [--client CAP --device UUID [--ds-max NUM] [--readonly]]\n\n"
"Options:\n"
" -v   Verbose mode.\n"
" -q   Quiet mode (do not print any warnings).\n"
" -d   Debug mode.\n"
" --client CAP    Add a static client via the CAP capability\n"
" --device UUID   Specify the UUID of the device\n"
" --ds-max NUM    Specify maximum number of dataspaces the client can register\n"
" --readonly      Only allow readonly access to the device";

struct Virtio_block_device_factory
{
  using Device_type = Virtio::Block_device;
  using Client_type = Block_device::Virtio_client<Device_type>;

  static cxx::unique_ptr<Client_type>
  create_client(cxx::Ref_ptr<Device_type> const &dev, unsigned numds, bool readonly)
  {
    return cxx::make_unique<Client_type>(dev, numds, readonly);
  }

  static cxx::Ref_ptr<Device_type>
  create_partition(cxx::Ref_ptr<Device_type> const &dev, unsigned partition_id,
                   Block_device::Partition_info const &pi)
  {
    return cxx::Ref_ptr<Device_type>(new Virtio::Partitioned_device(dev, partition_id, pi));
  }
};

using Base_device_mgr = Block_device::Device_mgr<Block_device::Device,
                                                 Virtio_block_device_factory>;

class Blk_mgr
: public Base_device_mgr,
  public L4::Epiface_t<Blk_mgr, L4::Factory>
{
  class Deletion_irq : public L4::Irqep_t<Deletion_irq>
  {
  public:
    void handle_irq()
    { _parent->check_clients(); }

    Deletion_irq(Blk_mgr *parent) : _parent{parent} {}

  private:
    Blk_mgr *_parent;
  };

public:
  Blk_mgr(L4Re::Util::Object_registry *registry)
  : Base_device_mgr(registry),
    _del_irq(this)
  {
    auto c = L4Re::chkcap(registry->register_irq_obj(&_del_irq),
                          "Creating IRQ for IPC gate deletion notifications.");
    L4Re::chksys(L4Re::Env::env()->main_thread()->register_del_irq(c),
                 "Registering deletion IRQ at the thread.");
  }

  long op_create(L4::Factory::Rights, L4::Ipc::Cap<void> &res, l4_umword_t,
                 L4::Ipc::Varg_list_ref valist)
  {
    Dbg::trace().printf("Client requests connection.\n");

    auto const *device_str = valist.get<char const *>();
    if (!device_str)
      {
        Dbg::warn().printf("Client did not provide device parameter.\n");
        return -L4_EINVAL;
      }

    std::string device;
    int ret = Base_device_mgr::parse_device_name(device_str, device);
    if (ret < 0)
      return ret;

    unsigned partno = -1;
    if (valist)
      partno = valist.get<unsigned>(-1);

    unsigned numds = 1;
    if (valist)
      numds = valist.get<unsigned>(1);

    bool readonly = false;
    if (valist)
      readonly = valist.get<bool>(false);

    Dbg::trace().printf("Client request: device=%s partno=%d numds=%u readonly=%d\n",
                        device.c_str(), partno, numds, readonly);

    ret = create_dynamic_client(device, partno, numds, &res, readonly);
    if (ret < 0)
      Dbg::warn().printf("Failed to create client: %d\n", ret);

    return ret;
  }

  void run()
  {
    // Handle events asynchronously
    server_loop();
  }

  L4::Factory *_factory() { return this; }

private:
  Deletion_irq _del_irq;
};

static void scan_virtio_devices(Blk_mgr *manager)
{
  Dbg::info().printf("Scanning for VirtIO devices...\n");

  auto *env = L4Re::Env::env();

  // Look for VirtIO block devices in the environment
  // QEMU exposes VirtIO devices via device tree or config

  // For simplicity, we'll search for virtio-blk devices
  // This would normally involve device tree parsing or enumeration

  // Create a VirtIO block device
  auto device = Virtio::Block_device::create();

  if (!device)
    {
      Dbg::warn().printf("No VirtIO block device found\n");
      return;
    }

  Dbg::info().printf("Found VirtIO block device\n");

  // Add device to manager
  manager->add_disk(std::move(device), []() {
    Dbg::info().printf("Disk scan completed\n");
  });
}

int main(int argc, char *argv[])
{
  // Set up error handling
  set_terminate_handler();

  bool verbose = false;
  bool quiet = false;
  bool debug = false;

  int opt;
  while ((opt = getopt(argc, argv, "vqd")) != -1)
    {
      switch (opt)
        {
        case 'v': verbose = true; break;
        case 'q': quiet = true; break;
        case 'd': debug = true; break;
        default:
          fprintf(stderr, usage_str, argv[0]);
          return 1;
        }
    }

  if (verbose) Dbg::set_level(Dbg::Trace);
  if (debug) Dbg::set_level(Dbg::Debug);
  if (quiet) Dbg::set_level(Dbg::Warn);

  printf("VirtIO Block Device Driver starting...\n");

  try
    {
      L4Re::Util::Br_manager brm;
      L4Re::Util::Object_registry registry(&brm);

      Blk_mgr mgr(&registry);
      auto factory_cap = registry.register_obj(&mgr);
      if (!factory_cap.is_valid())
        {
          fprintf(stderr, "Failed to register factory\n");
          return 1;
        }

      L4Re::Env::env()->register_lua("virtio-block", factory_cap);

      printf("VirtIO block driver registered\n");

      // Scan for VirtIO devices
      scan_virtio_devices(&mgr);

      // Run the server loop
      mgr.run();

    }
  catch (L4::Runtime_error &e)
    {
      fprintf(stderr, "Runtime error: %s\n", e.str());
      return 1;
    }
  catch (...)
    {
      fprintf(stderr, "Unknown error occurred\n");
      return 1;
    }

  printf("VirtIO Block Device Driver exiting\n");
  return 0;
}