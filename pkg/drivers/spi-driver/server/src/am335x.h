/*
 * AM335x McSPI controller driver.
 *
 * Register layout based on AM335x Technical Reference Manual (SPRUH73),
 * Chapter 24: McSPI.
 *
 * The AM335x has two McSPI modules:
 *   McSPI0: base 0x48030000, IRQ 65
 *   McSPI1: base 0x481A0000, IRQ 125
 *
 * Each module supports up to 4 channels (chip-selects).
 * FIFO depth: 32 bytes per channel for TX and RX.
 */
#pragma once

#include <l4/cxx/bitfield>
#include <l4/util/util.h>
#include <l4/drivers/hw_mmio_register_block>

#include "debug.h"
#include "controller.h"
#include "util.h"

#include <thread-l4>

namespace Am335x_spi {

static Dbg warn() { return Dbg(Dbg::Warn, "AM335x-SPI"); }
static Dbg info() { return Dbg(Dbg::Info, "AM335x-SPI"); }
static Dbg trace() { return Dbg(Dbg::Trace, "AM335x-SPI"); }

// McSPI register offsets (relative to module base)
enum Regs : unsigned
{
  SYSCONFIG    = 0x110,
  SYSSTATUS    = 0x114,
  IRQSTATUS    = 0x118,
  IRQENABLE    = 0x11C,
  MODULCTRL    = 0x128,

  // Per-channel offsets: base + 0x12C + ch * 0x14
  // CH0: CONF=0x12C, STAT=0x130, CTRL=0x134, TX=0x138, RX=0x13C
  // CH1: CONF=0x140, STAT=0x144, CTRL=0x148, TX=0x14C, RX=0x150
  // CH2: CONF=0x154, STAT=0x158, CTRL=0x15C, TX=0x160, RX=0x164
  // CH3: CONF=0x168, STAT=0x16C, CTRL=0x170, TX=0x174, RX=0x178
};

static constexpr unsigned ch_conf(unsigned ch) { return 0x12C + ch * 0x14; }
static constexpr unsigned ch_stat(unsigned ch) { return 0x130 + ch * 0x14; }
static constexpr unsigned ch_ctrl(unsigned ch) { return 0x134 + ch * 0x14; }
static constexpr unsigned ch_tx(unsigned ch)   { return 0x138 + ch * 0x14; }
static constexpr unsigned ch_rx(unsigned ch)   { return 0x13C + ch * 0x14; }

// MCSPI_CHxCONF bitfields
struct Chconf
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(29, 29, clkg, raw);       // Clock granularity
  CXX_BITFIELD_MEMBER(28, 28, ffer, raw);        // FIFO enable for RX
  CXX_BITFIELD_MEMBER(27, 27, ffew, raw);        // FIFO enable for TX
  CXX_BITFIELD_MEMBER(25, 26, tcs, raw);         // CS timing control
  CXX_BITFIELD_MEMBER(23, 24, sbpol, raw);       // Start bit polarity
  CXX_BITFIELD_MEMBER(21, 22, sbe, raw);         // Start bit enable
  CXX_BITFIELD_MEMBER(20, 20, force, raw);       // CS force
  CXX_BITFIELD_MEMBER(18, 19, turbo, raw);       // Turbo mode
  CXX_BITFIELD_MEMBER(16, 16, is, raw);          // Input select (D0/D1)
  CXX_BITFIELD_MEMBER(15, 15, dpe1, raw);        // D1 TX enable (0=TX on D1)
  CXX_BITFIELD_MEMBER(14, 14, dpe0, raw);        // D0 TX enable
  CXX_BITFIELD_MEMBER(13, 13, dmaw, raw);        // DMA write
  CXX_BITFIELD_MEMBER(12, 12, dmar, raw);        // DMA read
  CXX_BITFIELD_MEMBER(7, 11, wl, raw);           // Word length (0-31 = 1-32 bits)
  CXX_BITFIELD_MEMBER(6, 6, epol, raw);          // CS polarity (0=active high)
  CXX_BITFIELD_MEMBER(2, 5, clkd, raw);          // Clock divider
  CXX_BITFIELD_MEMBER(1, 1, pol, raw);           // CPOL
  CXX_BITFIELD_MEMBER(0, 0, pha, raw);           // CPHA
};

// MCSPI_CHxSTAT bitfields
struct Chstat
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(6, 6, eot, raw);     // End of transfer
  CXX_BITFIELD_MEMBER(5, 5, txffe, raw);   // TX FIFO empty
  CXX_BITFIELD_MEMBER(4, 4, txfff, raw);   // TX FIFO full
  CXX_BITFIELD_MEMBER(3, 3, rxffe, raw);   // RX FIFO empty
  CXX_BITFIELD_MEMBER(2, 2, rxfff, raw);   // RX FIFO full
  CXX_BITFIELD_MEMBER(1, 1, txs, raw);     // TX register empty
  CXX_BITFIELD_MEMBER(0, 0, rxs, raw);     // RX register full
};

// MCSPI_CHxCTRL bitfields
struct Chctrl
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(8, 15, extclk, raw); // Clock ratio ext (CLKG=1)
  CXX_BITFIELD_MEMBER(0, 0, en, raw);      // Channel enable
};

// MCSPI_MODULCTRL bitfields
struct Modulctrl
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(4, 4, fdaa, raw);    // FIFO DMA address alignment
  CXX_BITFIELD_MEMBER(3, 3, moa, raw);     // Multiple word OCP access
  CXX_BITFIELD_MEMBER(2, 2, pin34, raw);   // Pin mode (3/4 wire)
  CXX_BITFIELD_MEMBER(1, 1, ms, raw);      // Master/Slave (0=master)
  CXX_BITFIELD_MEMBER(0, 0, single, raw);  // Single channel mode
};

// MCSPI_IRQSTATUS / MCSPI_IRQENABLE bit positions per channel
// CH0: TX0_EMPTY=0, TX0_UNDERFLOW=1, RX0_FULL=2, RX0_OVERFLOW=3, EOW=17
static constexpr l4_uint32_t irq_tx_empty(unsigned ch) { return 1U << (ch * 4); }
static constexpr l4_uint32_t irq_rx_full(unsigned ch)  { return 1U << (ch * 4 + 2); }
static constexpr l4_uint32_t Irq_eow = 1U << 17; // End of word count


class Ctrl_am335x : public Ctrl_base
{
public:
  Ctrl_am335x() = default;

  bool probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu) override;
  char const *name() override { return _compatible[0]; }

  void setup(L4Re::Util::Object_registry *registry) override;
  long transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                unsigned len) override;
  long read(unsigned cs, l4_uint8_t *buf, unsigned len) override;
  long write(unsigned cs, l4_uint8_t const *buf, unsigned len) override;
  long configure(unsigned cs, l4_uint8_t mode, l4_uint32_t speed_hz) override;

private:
  l4_uint32_t read32(unsigned reg)
  { return _regs.read<l4_uint32_t>(reg); }

  void write32(unsigned reg, l4_uint32_t val)
  { _regs.write<l4_uint32_t>(val, reg); }

  unsigned wfi()
  {
    static l4_timeout_t timeout_100ms =
      l4_timeout(l4_timeout_from_us(100'000), l4_timeout_from_us(100'000));

    unsigned err = l4_ipc_error(_irq->receive(timeout_100ms), l4_utcb());
    if (err)
      {
        long errn = l4_ipc_to_errno(err);
        warn().printf("Error during wait for interrupt: %s (%li)\n",
                      l4sys_errtostr(errn), errn);
      }
    return err;
  }

  void soft_reset();
  void set_mode(unsigned cs, l4_uint8_t mode);
  void set_speed(unsigned cs, l4_uint32_t speed_hz);
  long do_transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                   unsigned len);

  void error_handler(unsigned cs)
  {
    // Disable channel, clear IRQ status
    Chctrl ctrl;
    ctrl.raw = read32(ch_ctrl(cs));
    ctrl.en() = 0;
    write32(ch_ctrl(cs), ctrl.raw);

    write32(IRQSTATUS, 0xFFFFFFFFU);
  }

  void alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu,
                            L4vbus::Device &dev, l4vbus_device_t &devinfo,
                            l4_addr_t &base, l4_addr_t &end,
                            L4::Cap<L4::Irq> &irq, bool &unmask_at_irq,
                            Dbg const &dbg);

  L4drivers::Mmio_register_block<32> _regs;
  l4_addr_t _end;
  L4::Cap<L4::Irq> _irq;
  bool _unmask_at_irq = false;

  // AM335x McSPI reference clock is 48 MHz
  static constexpr l4_uint32_t Ref_clk = 48000000;
  static constexpr unsigned Max_cs = 4;

  char const *_compatible[2] = {"ti,omap4-mcspi", "ti,omap2-mcspi"};
};


void
Ctrl_am335x::soft_reset()
{
  // Set soft-reset bit
  write32(SYSCONFIG, 0x2);

  // Wait for reset complete (SYSSTATUS bit 0)
  for (int i = 0; i < 1000; ++i)
    {
      if (read32(SYSSTATUS) & 0x1)
        break;
      l4_sleep(1);
    }

  // Set master mode, multi-channel
  Modulctrl mc;
  mc.ms() = 0;      // master
  mc.single() = 0;  // multi-channel
  write32(MODULCTRL, mc.raw);
}

void
Ctrl_am335x::setup(L4Re::Util::Object_registry *)
{
  L4Re::chksys(_irq->bind_thread(Pthread::L4::cap(pthread_self()), 0x5d1a335),
               "Failed to bind controller IRQ to thread.");

  soft_reset();

  // Configure all channels with default settings (8-bit, mode 0)
  for (unsigned cs = 0; cs < Max_cs; ++cs)
    {
      Chconf conf;
      conf.wl() = 7;          // 8-bit word length
      conf.epol() = 1;        // CS active low
      conf.dpe0() = 1;        // Disable TX on D0
      conf.dpe1() = 0;        // Enable TX on D1 (MOSI)
      conf.is() = 0;          // RX on D0 (MISO)
      conf.force() = 1;       // Force CS via software
      write32(ch_conf(cs), conf.raw);

      // Disable channel initially
      Chctrl ctrl;
      ctrl.en() = 0;
      write32(ch_ctrl(cs), ctrl.raw);
    }

  // Disable all interrupts initially, clear pending
  write32(IRQENABLE, 0);
  write32(IRQSTATUS, 0xFFFFFFFFU);

  if (_unmask_at_irq)
    L4Re::chkipc(_irq->unmask(), "Unmask IRQ\n");
}

void
Ctrl_am335x::set_mode(unsigned cs, l4_uint8_t mode)
{
  Chconf conf;
  conf.raw = read32(ch_conf(cs));

  conf.pol() = (mode >> 1) & 1; // CPOL
  conf.pha() = mode & 1;        // CPHA

  write32(ch_conf(cs), conf.raw);
}

void
Ctrl_am335x::set_speed(unsigned cs, l4_uint32_t speed_hz)
{
  if (speed_hz == 0)
    return;

  // McSPI clock divider: SPI_CLK = Ref_clk / 2^clkd
  // Find smallest clkd such that Ref_clk / 2^clkd <= speed_hz
  unsigned clkd = 0;
  while (clkd < 15 && (Ref_clk >> (clkd + 1)) >= speed_hz)
    clkd++;

  Chconf conf;
  conf.raw = read32(ch_conf(cs));
  conf.clkd() = clkd;
  conf.clkg() = 0; // power-of-two granularity
  write32(ch_conf(cs), conf.raw);

  trace().printf("Speed config CS%u: clkd=%u, actual=%u Hz\n",
                 cs, clkd, Ref_clk >> clkd);
}

long
Ctrl_am335x::configure(unsigned cs, l4_uint8_t mode, l4_uint32_t speed_hz)
{
  if (cs >= Max_cs)
    return -L4_EINVAL;
  if (mode > 3)
    return -L4_EINVAL;

  set_mode(cs, mode);
  set_speed(cs, speed_hz);

  info().printf("Configured CS%u: mode=%u, speed=%u Hz\n", cs, mode, speed_hz);
  return L4_EOK;
}

long
Ctrl_am335x::do_transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                         unsigned len)
{
  if (cs >= Max_cs)
    return -L4_EINVAL;

  // Enable IRQ for TX empty and RX full on this channel
  l4_uint32_t irq_mask = irq_tx_empty(cs) | irq_rx_full(cs);
  write32(IRQENABLE, irq_mask);
  write32(IRQSTATUS, 0xFFFFFFFFU);

  // Assert CS (force bit should already be set in CONF)
  Chconf conf;
  conf.raw = read32(ch_conf(cs));
  conf.force() = 1;
  write32(ch_conf(cs), conf.raw);

  // Enable channel
  Chctrl ctrl;
  ctrl.en() = 1;
  write32(ch_ctrl(cs), ctrl.raw);

  unsigned tx_count = 0;
  unsigned rx_count = 0;

  while (rx_count < len)
    {
      Chstat stat;
      stat.raw = read32(ch_stat(cs));

      // Write TX data when TX register is empty
      while (tx_count < len && stat.txs())
        {
          write32(ch_tx(cs), tx ? tx[tx_count] : 0);
          tx_count++;
          stat.raw = read32(ch_stat(cs));
        }

      // Read RX data when RX register is full
      while (rx_count < len && stat.rxs())
        {
          l4_uint8_t byte = read32(ch_rx(cs)) & 0xFF;
          if (rx)
            rx[rx_count] = byte;
          rx_count++;
          stat.raw = read32(ch_stat(cs));
        }

      if (rx_count < len)
        {
          if (wfi())
            {
              error_handler(cs);
              return -L4_EIO;
            }

          // Clear handled IRQ bits
          write32(IRQSTATUS, read32(IRQSTATUS));
        }
    }

  // Disable channel
  ctrl.en() = 0;
  write32(ch_ctrl(cs), ctrl.raw);

  // Deassert CS
  conf.raw = read32(ch_conf(cs));
  conf.force() = 0;
  write32(ch_conf(cs), conf.raw);

  // Disable interrupts
  write32(IRQENABLE, 0);
  write32(IRQSTATUS, 0xFFFFFFFFU);

  return L4_EOK;
}

long
Ctrl_am335x::transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                      unsigned len)
{
  return do_transfer(cs, tx, rx, len);
}

long
Ctrl_am335x::read(unsigned cs, l4_uint8_t *buf, unsigned len)
{
  return do_transfer(cs, nullptr, buf, len);
}

long
Ctrl_am335x::write(unsigned cs, l4_uint8_t const *buf, unsigned len)
{
  return do_transfer(cs, buf, nullptr, len);
}

void
Ctrl_am335x::alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus,
                                  L4::Cap<L4::Icu> icu,
                                  L4vbus::Device &dev,
                                  l4vbus_device_t &devinfo,
                                  l4_addr_t &base, l4_addr_t &end,
                                  L4::Cap<L4::Irq> &irq,
                                  bool &unmask_at_irq,
                                  Dbg const &dbg)
{
  base = 0;
  end = 0;
  irq = L4::Cap<L4::Irq>();

  for (unsigned i = 0; i < devinfo.num_resources; ++i)
    {
      l4vbus_resource_t res;

      L4Re::chksys(dev.get_resource(i, &res), "Get vbus device resources");

      switch(res.type)
        {
        case L4VBUS_RESOURCE_MEM:
          {
            alloc_mem_resource(vbus, res, base, end, dbg);
            break;
          }
        case L4VBUS_RESOURCE_IRQ:
          {
            alloc_irq_resource(icu, res, irq, dbg, unmask_at_irq);
            break;
          }
        default:
          dbg.printf("Unhandled resource type %u found: [0x%lx, 0x%lx], "
                     "flags: 0x%x\n",
                     res.type, res.start, res.end, res.flags);
        }
    }
}

bool
Ctrl_am335x::probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu)
{
  L4vbus::Device dev;
  l4vbus_device_t devinfo;

  bool matched = false;
  for (char const *c : _compatible)
    if (find_compatible(c, vbus, dev, devinfo))
      {
        matched = true;
        break;
      }

  if (!matched)
    return false;

  info().printf("Found a %s device.\n", name());

  l4_addr_t base = 0UL;
  alloc_ctrl_resources(vbus, icu, dev, devinfo, base, _end, _irq,
                       _unmask_at_irq, info());
  _regs.set_base(base);

  info().printf("MMIO resource [0x%lx, 0x%lx]; IRQ %s\n", base, _end,
                _irq.is_valid() ? "Yes" : "No");

  return true;
}

} // namespace Am335x_spi
