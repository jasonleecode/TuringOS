/*
 * Copyright (C) 2009 Technische Universität Dresden.
 * Author(s): Adam Lackorzynski <adam@os.inf.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

/*!
 * \file   support_beagleboard.cc
 * \brief  Support for the Beagleboard
 */

#include "support.h"
#include "startup.h"
#include "platform-arm.h"
#include <l4/drivers/uart_omap35x.h>

#ifdef PLATFORM_TYPE_omap3_am33xx
// AM335x Clock Module registers (SPRUH73 Ch8)
enum {
  Am33xx_cm_wkup_base  = 0x44e00400,
  Am33xx_cm_per_base   = 0x44e00000,

  // CM_WKUP offsets
  Cm_wkup_uart0_clkctrl  = 0xb4,

  // CM_PER offsets
  Cm_per_gpio1_clkctrl   = 0xac,
  Cm_per_i2c1_clkctrl    = 0x48,
  Cm_per_spi0_clkctrl    = 0x4c,
  Cm_per_timer2_clkctrl  = 0x80,

  Clkctrl_modulemode_enable = 0x2,
};

static inline void am33xx_write32(unsigned long addr, unsigned val)
{
  *reinterpret_cast<volatile unsigned *>(addr) = val;
}

static inline unsigned am33xx_read32(unsigned long addr)
{
  return *reinterpret_cast<volatile unsigned *>(addr);
}

static void am33xx_clk_enable(unsigned long base, unsigned offset)
{
  am33xx_write32(base + offset, Clkctrl_modulemode_enable);
  // Wait until module is fully functional (IDLEST bits[17:16] == 0)
  for (int i = 0; i < 10000; ++i)
    if (!(am33xx_read32(base + offset) & (0x3 << 16)))
      break;
}

static void am33xx_clocks_init()
{
  am33xx_clk_enable(Am33xx_cm_wkup_base, Cm_wkup_uart0_clkctrl);
  am33xx_clk_enable(Am33xx_cm_per_base,  Cm_per_gpio1_clkctrl);
  am33xx_clk_enable(Am33xx_cm_per_base,  Cm_per_i2c1_clkctrl);
  am33xx_clk_enable(Am33xx_cm_per_base,  Cm_per_spi0_clkctrl);
  am33xx_clk_enable(Am33xx_cm_per_base,  Cm_per_timer2_clkctrl);
}
#endif // PLATFORM_TYPE_omap3_am33xx

namespace {
class Platform_arm_omap : public Platform_single_region_ram<Platform_arm>
{
  bool probe() override { return true; }

  void init() override
  {
    kuart.baud         = 115200;
    kuart.access_type  = L4_kernel_options::Uart_type_mmio;
#ifdef PLATFORM_TYPE_beagleboard
    kuart.base_address = 0x49020000;
    kuart.irqno = 74;
#elif defined(PLATFORM_TYPE_omap3evm)
    kuart.base_address = 0x4806a000;
    kuart.irqno = 72;
#elif defined(PLATFORM_TYPE_omap3_am33xx)
    am33xx_clocks_init();
    kuart.base_address = 0x44e09000;
    kuart.irqno = 72;
#elif defined(PLATFORM_TYPE_pandaboard) || defined(PLATFORM_TYPE_omap5)
    kuart.base_address = 0x48020000;
    kuart.irqno = 32 + 74;
#else
#error Unknown platform
#endif

    kuart_flags       |=   L4_kernel_options::F_uart_base
                         | L4_kernel_options::F_uart_baud
                         | L4_kernel_options::F_uart_irq;

    static L4::Uart_omap35x _uart;
    static L4::Io_register_block_mmio r(kuart.base_address);
    _uart.startup(&r);
    set_stdio_uart(&_uart);
  }

  bool arm_switch_to_hyp() override
  {
    register l4_umword_t f asm("r12") = 0x102;
    asm volatile("push {fp}                 \n"
                 "mcr p15, 0, sp, c13, c0, 2\n"
                 "mov r0, pc                \n"
                 ".inst 0xe1600071          \n"
                 "mrc p15, 0, sp, c13, c0, 2\n"
                 "pop {fp}                  \n"
                 :
                 : "r" (f)
                 : "r0", "r1", "r2", "r3", "r4",
                   "r5", "r6", "r7", "r8", "r9",
                   "r10", "memory");

    return true;
  }
};
}

REGISTER_PLATFORM(Platform_arm_omap);
