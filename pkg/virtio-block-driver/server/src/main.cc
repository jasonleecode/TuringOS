/*
 * VirtIO block device driver for TuringOS – main entry point
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Follows the same structure as ahci-driver and nvme-driver:
 *   1. Parse arguments / static client options
 *   2. set_server_iface()
 *   3. Enumerate vbus for virtio-mmio block devices
 *   4. server.loop()
 */

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <getopt.h>
#include <map>
#include <string>
#include <vector>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/shared_cap>
#include <l4/re/dma_space>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_inhibitor.h>
#include <l4/sys/task>

#include <terminate_handler-l4>

// debug.h must come before libblock-device headers
#include "debug.h"
#include <l4/libblock-device/block_device_mgr.h>
#include <l4/libblock-device/virtio_client.h>

#include "virtio_device.h"

// -----------------------------------------------------------------------
// Usage string
// -----------------------------------------------------------------------

static char const *const usage_str =
"Usage: %s [-vq] [--client CAP --device NAME [--ds-max NUM] [--readonly]]\n\n"
"Options:\n"
" -v            Verbose mode\n"
" -q            Quiet mode\n"
" --client CAP  Add a static client via CAP capability\n"
" --device NAME Device name to present to client (e.g. 'virtio-blk0')\n"
" --ds-max NUM  Maximum number of dataspaces a client may register\n"
" --readonly    Give the client read-only access\n";

// -----------------------------------------------------------------------
// Device manager
// -----------------------------------------------------------------------

using Base_device_mgr =
  Block_device::Device_mgr<Block_device::Device,
                            Block_device::Partitionable_factory<Block_device::Device>>;

class Blk_mgr
: public Base_device_mgr,
  public L4::Epiface_t<Blk_mgr, L4::Factory>
{
  class Deletion_irq : public L4::Irqep_t<Deletion_irq>
  {
  public:
    void handle_irq()
    { _parent->check_clients(); }
    explicit Deletion_irq(Blk_mgr *p) : _parent(p) {}
  private:
    Blk_mgr *_parent;
  };

public:
  explicit Blk_mgr(L4Re::Util::Object_registry *registry)
  : Base_device_mgr(registry), _del_irq(this)
  {
    auto c = L4Re::chkcap(registry->register_irq_obj(&_del_irq),
                          "Create deletion IRQ");
    L4Re::chksys(L4Re::Env::env()->main_thread()->register_del_irq(c),
                 "Register deletion IRQ");
  }

  long op_create(L4::Factory::Rights, L4::Ipc::Cap<void> &res,
                 l4_umword_t, L4::Ipc::Varg_list_ref valist)
  {
    Dbg::trace().printf("Dynamic client connect\n");

    std::string device;
    int  num_ds   = 2;
    bool readonly = false;

    for (L4::Ipc::Varg p : valist)
      {
        if (!p.is_of<char const *>())
          {
            Dbg::warn().printf("String parameter expected\n");
            return -L4_EINVAL;
          }

        std::string dev_param;
        if (parse_string_param(p, "device=", &dev_param))
          {
            long ret = parse_device_name(dev_param, device);
            if (ret < 0) return ret;
            continue;
          }
        if (parse_int_param(p, "ds-max=", &num_ds))
          {
            if (num_ds <= 0 || num_ds > 256)
              {
                Dbg::warn().printf("ds-max out of range\n");
                return -L4_EINVAL;
              }
            continue;
          }
        if (strncmp(p.value<char const *>(), "read-only", p.length()) == 0)
          readonly = true;
      }

    if (device.empty())
      {
        Dbg::warn().printf("No device= parameter\n");
        return -L4_EINVAL;
      }

    L4::Cap<void> cap;
    int ret = create_dynamic_client(device, -1, num_ds, &cap, readonly);
    if (ret >= 0)
      {
        res = L4::Ipc::make_cap(cap, L4_CAP_FPAGE_RWSD);
        L4::cap_cast<L4::Kobject>(cap)->dec_refcnt(1);
      }

    return (ret == -L4_ENODEV && _scan_in_progress) ? -L4_EAGAIN : ret;
  }

  void scan_finished() { _scan_in_progress = false; }

private:
  static bool parse_string_param(L4::Ipc::Varg const &p, char const *prefix,
                                  std::string *out)
  {
    l4_size_t plen = strlen(prefix);
    if (p.length() < plen) return false;
    char const *s = p.value<char const *>();
    if (strncmp(s, prefix, plen) != 0) return false;
    *out = std::string(s + plen, strnlen(s, p.length()) - plen);
    return true;
  }

  static bool parse_int_param(L4::Ipc::Varg const &p, char const *prefix,
                               int *out)
  {
    l4_size_t plen = strlen(prefix);
    if (p.length() < plen) return false;
    char const *s = p.value<char const *>();
    if (strncmp(s, prefix, plen) != 0) return false;
    std::string tail(s + plen, p.length() - plen);
    char *end;
    long val = strtol(tail.c_str(), &end, 10);
    if (*end != '\0') { Dbg::warn().printf("Bad int param\n"); return false; }
    *out = (int)val;
    return true;
  }

  Deletion_irq _del_irq;
  bool         _scan_in_progress = true;
};

// -----------------------------------------------------------------------
// Static client options
// -----------------------------------------------------------------------

struct Client_opts
{
  bool add_client(Blk_mgr *mgr)
  {
    if (!capname) return true;

    if (device.empty())
      {
        Err().printf("No device for client '%s'\n", capname);
        return false;
      }

    auto cap = L4Re::Env::env()->get_cap<L4::Rcv_endpoint>(capname);
    if (!cap.is_valid())
      {
        Err().printf("Client cap '%s' not found\n", capname);
        return false;
      }

    mgr->add_static_client(cap, device.c_str(), -1, ds_max, readonly);
    return true;
  }

  const char *capname = nullptr;
  std::string device;
  int  ds_max   = 2;
  bool readonly = false;
};

// -----------------------------------------------------------------------
// Hardware setup – vbus device enumeration
// -----------------------------------------------------------------------

static Block_device::Errand::Errand_server server;
static Blk_mgr drv(server.registry());

static unsigned devices_in_scan = 0;

static void device_scan_finished()
{
  if (--devices_in_scan > 0) return;

  drv.scan_finished();
  if (!server.registry()->register_obj(&drv, "svr").is_valid())
    Dbg::warn().printf("'svr' cap not found; no dynamic clients\n");
  else
    Dbg::info().printf("Accepting dynamic client connections\n");
}

static L4Re::Util::Shared_cap<L4Re::Dma_space>
create_dma_space(L4::Cap<L4vbus::Vbus> bus, unsigned long dma_id)
{
  static std::map<unsigned long, L4Re::Util::Shared_cap<L4Re::Dma_space>> cache;

  auto it = cache.find(dma_id);
  if (it != cache.end()) return it->second;

  auto dma = L4Re::chkcap(L4Re::Util::make_shared_cap<L4Re::Dma_space>(),
                          "Alloc DMA space cap");
  L4Re::chksys(L4Re::Env::env()->user_factory()->create(dma.get()),
               "Create DMA space");

  if (dma_id != ~0UL)
    {
      L4Re::chksys(
        bus->assign_dma_domain(dma_id,
                               L4VBUS_DMAD_BIND | L4VBUS_DMAD_L4RE_DMA_SPACE,
                               dma.get()),
        "Assign DMA domain");
    }
  else
    {
      // No IOMMU on QEMU virt ARM: associate directly with the physical
      // address space.  After this, dma->map() returns physical addresses
      // (identity mapping), which is what VirtIO MMIO QueuePFN expects.
      L4Re::chksys(
        dma->associate(L4::Ipc::Cap<L4::Task>::from_ci(L4_INVALID_CAP),
                       L4Re::Dma_space::Space_attrib::Phys_space),
        "Associate DMA space with physical address space");
    }

  cache[dma_id] = dma;
  return dma;
}

static void setup_hardware()
{
  auto vbus = L4Re::chkcap(L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus"),
                           "Get 'vbus' capability");

  // Obtain ICU from vbus
  L4vbus::Icu icudev;
  L4Re::chksys(vbus->root().device_by_hid(&icudev, "L40009"),
               "Find ICU in vbus");
  auto icu = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Icu>(),
                          "Alloc ICU cap");
  L4Re::chksys(icudev.vicu(icu), "Get ICU cap");

  ++devices_in_scan;  // prevent premature finish before loop ends

  L4vbus::Device dev;
  l4vbus_device_t di;
  unsigned found = 0;

  while (vbus->root().next_device(&dev, L4VBUS_MAX_DEPTH, &di) == L4_EOK)
    {
      if (dev.is_compatible("virtio,mmio") != 1)
        continue;

      // --- Collect resources ---
      l4_addr_t   mmio_start = 0;
      l4_size_t   mmio_size  = 0;
      unsigned    irq_num    = 0;
      unsigned long dma_id   = ~0UL;
      bool found_mmio = false, found_irq = false;

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
          else if (res.type == L4VBUS_RESOURCE_IRQ && !found_irq)
            {
              irq_num   = (unsigned)res.start;
              found_irq = true;
            }
          else if (res.type == L4VBUS_RESOURCE_DMA_DOMAIN)
            {
              dma_id = (unsigned long)res.start;
            }
        }

      if (!found_mmio || !found_irq)
        {
          Dbg::warn().printf("virtio,mmio device missing resources\n");
          continue;
        }

      // --- Map MMIO (uncached) ---
      // attach() snaps to the page boundary, so mmio_virt maps the page
      // containing mmio_start.  Compute the sub-page offset so that our
      // register pointer lands on the actual device registers.
      l4_addr_t page_offset = mmio_start & (L4_PAGESIZE - 1);
      auto iods =
        L4::Ipc::make_cap_rw(L4::cap_reinterpret_cast<L4Re::Dataspace>(vbus));
      l4_addr_t mmio_virt = 0;
      int r = L4Re::Env::env()->rm()->attach(
        &mmio_virt, mmio_size + page_offset,
        L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW
        | L4Re::Rm::F::Cache_uncached,
        iods, mmio_start - page_offset, L4_PAGESHIFT);
      if (r < 0)
        {
          Dbg::warn().printf("Failed to map MMIO 0x%lx (err %d)\n",
                             mmio_start, r);
          continue;
        }
      l4_addr_t dev_base = mmio_virt + page_offset;

      // --- Check DeviceID: only accept block devices (ID=2) ---
      l4_uint32_t dev_id =
        reinterpret_cast<volatile l4_uint32_t *>(dev_base)[2];
      if (dev_id != 2)
        {
          Dbg::trace().printf("virtio,mmio at 0x%lx is not a block device "
                              "(DeviceID=%u), skipping\n", mmio_start, dev_id);
          L4Re::Env::env()->rm()->detach(mmio_virt, nullptr);
          continue;
        }

      Dbg::info().printf("Found VirtIO block device at MMIO 0x%lx IRQ %u\n",
                         mmio_start, irq_num);

      // --- Create DMA space ---
      L4Re::Util::Shared_cap<L4Re::Dma_space> dma;
      try
        {
          dma = create_dma_space(vbus, dma_id);
        }
      catch (L4::Runtime_error const &e)
        {
          Err().printf("DMA space error: %s\n", e.str());
          L4Re::Env::env()->rm()->detach(mmio_virt, nullptr);
          continue;
        }

      // --- Instantiate and initialise device ---
      auto device = cxx::make_ref_obj<Virtio::Block_dev>(dev_base, dma, found);
      try
        {
          device->init();
          // IRQ trigger type from IO config: raise-edge → true
          device->register_irq(icu, server.registry(), irq_num, true);
        }
      catch (L4::Runtime_error const &e)
        {
          Err().printf("Device init error: %s\n", e.str());
          continue;
        }

      ++found;
      ++devices_in_scan;
      drv.add_disk(cxx::Ref_ptr<Block_device::Device>(device),
                   device_scan_finished);
    }

  if (found == 0)
    Dbg::warn().printf("No VirtIO block devices found\n");

  device_scan_finished();  // matches the guard increment at the top
}

// -----------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------

static int parse_args(int argc, char *const *argv)
{
  int debug_level = 1;

  enum { OPT_CLIENT = 256, OPT_DEVICE, OPT_DS_MAX, OPT_READONLY };

  static struct option const lopts[] = {
    { "verbose",  no_argument,       nullptr, 'v'         },
    { "quiet",    no_argument,       nullptr, 'q'         },
    { "client",   required_argument, nullptr, OPT_CLIENT  },
    { "device",   required_argument, nullptr, OPT_DEVICE  },
    { "ds-max",   required_argument, nullptr, OPT_DS_MAX  },
    { "readonly", no_argument,       nullptr, OPT_READONLY},
    { nullptr, 0, nullptr, 0 },
  };

  Client_opts opts;
  for (;;)
    {
      int opt = getopt_long(argc, argv, "vq", lopts, nullptr);
      if (opt == -1) break;

      switch (opt)
        {
        case 'v': debug_level = (debug_level << 1) | 1; break;
        case 'q': debug_level = 0; break;

        case OPT_CLIENT:
          if (!opts.add_client(&drv)) return -1;
          opts = Client_opts();
          opts.capname = optarg;
          break;

        case OPT_DEVICE:
          {
            long ret = Blk_mgr::parse_device_name(optarg, opts.device);
            if (ret < 0) { Dbg::warn().printf("Bad device name\n"); return -1; }
          }
          break;

        case OPT_DS_MAX:
          opts.ds_max = atoi(optarg);
          break;

        case OPT_READONLY:
          opts.readonly = true;
          break;

        default:
          fprintf(stderr, usage_str, argv[0]);
          return -1;
        }
    }

  if (!opts.add_client(&drv)) return -1;

  Dbg::set_level(debug_level);
  return optind;
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------

int main(int argc, char *const *argv)
{
  Dbg::set_level(3);

  int idx = parse_args(argc, argv);
  if (idx < 0) return idx;

  Dbg::info().printf("VirtIO block driver starting\n");

  Block_device::Errand::set_server_iface(&server);
  setup_hardware();

  Dbg::info().printf("Entering server loop\n");
  server.loop();

  return 0;
}
