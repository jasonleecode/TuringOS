/*
 * Resource allocation utility functions.
 */
#pragma once

#include <l4/vbus/vbus>
#include <l4/vbus/vbus_gpio>
#include <l4/sys/icu>
#include <l4/re/env>
#include <l4/re/util/unique_cap>
#include <l4/sys/factory>
#include <l4/re/error_helper>

#include <cstdio>

#include "debug.h"

static bool
find_compatible(char const *compat, L4::Cap<L4vbus::Vbus> vbus,
                L4vbus::Device &dev, l4vbus_device_t &devinfo)
{
  while (vbus->root().next_device(&dev, L4VBUS_MAX_DEPTH, &devinfo) == 0)
    if (dev.is_compatible(compat) == 1)
      return true;

  return false;
}

static void
alloc_mem_resource(L4::Cap<L4vbus::Vbus> vbus, l4vbus_resource_t &res,
                   l4_addr_t &base, l4_addr_t &end, Dbg const &dbg)
{
  if (base != 0)
    {
      dbg.printf("More than one MMIO resources found. ignoring\n");
      return;
    }

  auto iods =
    L4::Ipc::make_cap_rw(L4::cap_reinterpret_cast<L4Re::Dataspace>(vbus));

  l4_size_t sz = res.end - res.start + 1;
  l4_addr_t addr = 0;
  L4Re::Env const *env = L4Re::Env::env();
  L4Re::chksys(env->rm()->attach(&addr, sz,
                                 L4Re::Rm::F::Search_addr
                                   | L4Re::Rm::F::Cache_uncached
                                   | L4Re::Rm::F::RW,
                                 iods, res.start, L4_PAGESHIFT),
               "Attach MMIO memory");
  base = addr;
  end = addr + sz - 1;

  dbg.printf("Found MMIO resource [0x%lx, 0x%lx] (sz=0x%zx); mapped "
             "to 0x%lx\n",
             res.start, res.end, sz, base);
}

static void
alloc_irq_resource(L4::Cap<L4::Icu> icu, l4vbus_resource_t &res,
                   L4::Cap<L4::Irq> &irq, Dbg const &dbg, bool &unmask_at_irq)
{
  if (irq.is_valid())
    {
      dbg.printf("More than one IRQ resources found. ignoring\n");
      return;
    }

  unsigned irq_num = res.start;
  if (res.end - res.start > 1)
    L4Re::throw_error(-L4_EINVAL, "IRQ resource too large\n");

  L4_irq_mode irq_mode = L4_irq_mode(res.flags);

  auto irq_cap = L4Re::Util::make_unique_cap<L4::Irq>();
  L4Re::chksys(L4Re::Env::env()->factory()->create(irq_cap.get()),
               "Create device IRQ");
  L4Re::chksys(icu->set_mode(irq_num, irq_mode), "Set IRQ mode.");

  long ret = L4Re::chksys(l4_error(icu->bind(irq_num, irq_cap.get())),
                          "Bind interrupt to vbus ICU.");

  unmask_at_irq = ret == 0;
  if (!unmask_at_irq)
    icu->unmask(irq_num);

  irq = irq_cap.release();
  dbg.printf("Found IRQ resource [0x%lx, 0x%lx] (sz=%lu) unmask at %s\n",
             res.start, res.end, res.end - res.start + 1,
             unmask_at_irq ? "IRQ" : "ICU");
}
