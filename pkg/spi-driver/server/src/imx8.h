/*
 * i.MX8 eCSPI controller driver.
 *
 * Register layout based on i.MX 8M Plus Applications Processor Reference Manual.
 */
#pragma once

#include <l4/cxx/bitfield>
#include <l4/util/util.h>
#include <l4/drivers/hw_mmio_register_block>
#include <l4/re/util/object_registry>

#include "debug.h"
#include "controller.h"
#include "util.h"

#include <thread-l4>
#include <cassert>

namespace Imx8_spi
{

static Dbg warn() { return Dbg(Dbg::Warn, "IMX8-SPI"); }
static Dbg info() { return Dbg(Dbg::Info, "IMX8-SPI"); }
static Dbg trace() { return Dbg(Dbg::Trace, "IMX8-SPI"); }

// eCSPI register offsets
enum Regs : unsigned
{
  RXDATA    = 0x00,
  TXDATA    = 0x04,
  CONREG    = 0x08,
  CONFIGREG = 0x0C,
  INTREG    = 0x10,
  DMAREG    = 0x14,
  STATREG   = 0x18,
  PERIODREG = 0x1C,
  TESTREG   = 0x20,
};

// CONREG bitfields
struct Conreg
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(0, 0, en, raw);          // SPI enable
  CXX_BITFIELD_MEMBER(1, 1, ht, raw);          // Hardware trigger
  CXX_BITFIELD_MEMBER(2, 2, xch, raw);         // Exchange bit (start transfer)
  CXX_BITFIELD_MEMBER(3, 3, smc, raw);         // Start mode control
  CXX_BITFIELD_MEMBER(4, 7, channel_mode, raw); // Channel mode (master/slave per ch)
  CXX_BITFIELD_MEMBER(8, 11, post_divider, raw);
  CXX_BITFIELD_MEMBER(12, 15, pre_divider, raw);
  CXX_BITFIELD_MEMBER(16, 17, drctl, raw);     // SPI data ready control
  CXX_BITFIELD_MEMBER(18, 19, channel_select, raw); // SPI channel select
  CXX_BITFIELD_MEMBER(20, 31, burst_length, raw);   // Burst length in bits - 1
};

// CONFIGREG bitfields
struct Configreg
{
  l4_uint32_t raw = 0;

  // Per-channel CPHA (bits 0-3)
  CXX_BITFIELD_MEMBER(0, 0, sclk_pha_ch0, raw);
  CXX_BITFIELD_MEMBER(1, 1, sclk_pha_ch1, raw);
  CXX_BITFIELD_MEMBER(2, 2, sclk_pha_ch2, raw);
  CXX_BITFIELD_MEMBER(3, 3, sclk_pha_ch3, raw);

  // Per-channel CPOL (bits 4-7)
  CXX_BITFIELD_MEMBER(4, 4, sclk_pol_ch0, raw);
  CXX_BITFIELD_MEMBER(5, 5, sclk_pol_ch1, raw);
  CXX_BITFIELD_MEMBER(6, 6, sclk_pol_ch2, raw);
  CXX_BITFIELD_MEMBER(7, 7, sclk_pol_ch3, raw);

  // Per-channel SS polarity (bits 12-15)
  CXX_BITFIELD_MEMBER(12, 12, ss_pol_ch0, raw);
  CXX_BITFIELD_MEMBER(13, 13, ss_pol_ch1, raw);
  CXX_BITFIELD_MEMBER(14, 14, ss_pol_ch2, raw);
  CXX_BITFIELD_MEMBER(15, 15, ss_pol_ch3, raw);

  // Per-channel SS control (bits 16-19): 0=one burst, 1=multi-burst
  CXX_BITFIELD_MEMBER(16, 16, ss_ctl_ch0, raw);
  CXX_BITFIELD_MEMBER(17, 17, ss_ctl_ch1, raw);
  CXX_BITFIELD_MEMBER(18, 18, ss_ctl_ch2, raw);
  CXX_BITFIELD_MEMBER(19, 19, ss_ctl_ch3, raw);

  // Per-channel DATA control (bits 20-23)
  CXX_BITFIELD_MEMBER(20, 20, data_ctl_ch0, raw);
  CXX_BITFIELD_MEMBER(21, 21, data_ctl_ch1, raw);
  CXX_BITFIELD_MEMBER(22, 22, data_ctl_ch2, raw);
  CXX_BITFIELD_MEMBER(23, 23, data_ctl_ch3, raw);

  // Per-channel SCLK control (bits 24-27)
  CXX_BITFIELD_MEMBER(24, 24, sclk_ctl_ch0, raw);
  CXX_BITFIELD_MEMBER(25, 25, sclk_ctl_ch1, raw);
  CXX_BITFIELD_MEMBER(26, 26, sclk_ctl_ch2, raw);
  CXX_BITFIELD_MEMBER(27, 27, sclk_ctl_ch3, raw);
};

// STATREG bitfields
struct Statreg
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(0, 0, te, raw);     // TX FIFO empty
  CXX_BITFIELD_MEMBER(1, 1, th, raw);     // TX FIFO half empty
  CXX_BITFIELD_MEMBER(2, 2, tf, raw);     // TX FIFO full
  CXX_BITFIELD_MEMBER(3, 3, rr, raw);     // RX FIFO ready (has data)
  CXX_BITFIELD_MEMBER(4, 4, rh, raw);     // RX FIFO half full
  CXX_BITFIELD_MEMBER(5, 5, rf, raw);     // RX FIFO full
  CXX_BITFIELD_MEMBER(6, 6, ro, raw);     // RX FIFO overflow
  CXX_BITFIELD_MEMBER(7, 7, tc, raw);     // Transfer complete
};

// INTREG bitfields
struct Intreg
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(0, 0, teen, raw);   // TX FIFO empty interrupt enable
  CXX_BITFIELD_MEMBER(1, 1, then_, raw);  // TX FIFO half interrupt enable
  CXX_BITFIELD_MEMBER(2, 2, tfen, raw);   // TX FIFO full interrupt enable
  CXX_BITFIELD_MEMBER(3, 3, rren, raw);   // RX FIFO ready interrupt enable
  CXX_BITFIELD_MEMBER(4, 4, rhen, raw);   // RX FIFO half interrupt enable
  CXX_BITFIELD_MEMBER(5, 5, rfen, raw);   // RX FIFO full interrupt enable
  CXX_BITFIELD_MEMBER(6, 6, roen, raw);   // RX FIFO overflow interrupt enable
  CXX_BITFIELD_MEMBER(7, 7, tcen, raw);   // Transfer complete interrupt enable
};

/// i.MX8 eCSPI controller implementation
class Ctrl_imx8 : public Ctrl_base
{
public:
  Ctrl_imx8() = default;

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

  void reset();
  void enable();
  void error_handler();
  long do_transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                   unsigned len);
  void set_channel_mode(unsigned cs, l4_uint8_t mode);
  void set_channel_speed(unsigned cs, l4_uint32_t speed_hz);

  void alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu,
                            L4vbus::Device &dev, l4vbus_device_t &devinfo,
                            l4_addr_t &base, l4_addr_t &end,
                            L4::Cap<L4::Irq> &irq, bool &unmask_at_irq,
                            Dbg const &dbg);

  void dump_ctrl_state()
  {
    warn().printf("Controller state:\n");
    warn().printf("  CONREG    (0x%02x): 0x%08x\n", CONREG, read32(CONREG));
    warn().printf("  CONFIGREG (0x%02x): 0x%08x\n", CONFIGREG, read32(CONFIGREG));
    warn().printf("  STATREG   (0x%02x): 0x%08x\n", STATREG, read32(STATREG));
    warn().printf("  INTREG    (0x%02x): 0x%08x\n", INTREG, read32(INTREG));
  }

  L4drivers::Mmio_register_block<32> _regs;
  l4_addr_t _end;
  L4::Cap<L4::Irq> _irq;
  bool _unmask_at_irq = false;

  char const *_compatible[2] = {"fsl,imx8mp-ecspi", "fsl,imx51-ecspi"};
};


void
Ctrl_imx8::setup(L4Re::Util::Object_registry *)
{
  L4Re::chksys(_irq->bind_thread(Pthread::L4::cap(pthread_self()), 0x5d1ec5d),
               "Failed to bind to controller IRQ to thread.");
  L4Re::chkipc(_irq->unmask(), "Failed to unmask controller IRQ.");

  reset();
  enable();
}

void
Ctrl_imx8::reset()
{
  // Disable the controller
  Conreg con;
  con.raw = read32(CONREG);
  con.en() = 0;
  write32(CONREG, con.raw);

  // Clear status
  Statreg sta;
  sta.raw = read32(STATREG);
  sta.tc() = 1;   // W1C: clear transfer complete
  sta.ro() = 1;   // W1C: clear RX overflow
  write32(STATREG, sta.raw);

  // Disable all interrupts
  write32(INTREG, 0);
}

void
Ctrl_imx8::enable()
{
  Conreg con;
  con.raw = read32(CONREG);
  con.en() = 1;
  // Set all channels as master
  con.channel_mode() = 0xf;
  // Default burst length: 8 bits (= 7 in register)
  con.burst_length() = 7;
  write32(CONREG, con.raw);

  // Enable transfer complete interrupt
  Intreg intr;
  intr.raw = 0;
  intr.tcen() = 1;
  write32(INTREG, intr.raw);
}

void
Ctrl_imx8::error_handler()
{
  reset();
  enable();
}

void
Ctrl_imx8::set_channel_mode(unsigned cs, l4_uint8_t mode)
{
  Configreg cfg;
  cfg.raw = read32(CONFIGREG);

  l4_uint8_t cpha = mode & 0x1;
  l4_uint8_t cpol = (mode >> 1) & 0x1;

  // Set per-channel CPHA (bits 0-3) and CPOL (bits 4-7)
  cfg.raw &= ~(1U << cs);       // clear CPHA bit for this channel
  cfg.raw |= (cpha << cs);
  cfg.raw &= ~(1U << (cs + 4)); // clear CPOL bit for this channel
  cfg.raw |= (cpol << (cs + 4));

  write32(CONFIGREG, cfg.raw);
}

void
Ctrl_imx8::set_channel_speed(unsigned cs, l4_uint32_t speed_hz)
{
  (void)cs; // speed is set globally in eCSPI via CONREG dividers

  if (speed_hz == 0)
    return;

  // eCSPI clock rate = reference clock / (pre_divider * 2^post_divider)
  // Assuming reference clock of 60 MHz (typical for i.MX8)
  l4_uint32_t ref_clk = 60000000;
  l4_uint32_t div = ref_clk / speed_hz;

  // Find best pre/post divider combination
  l4_uint32_t post = 0;
  l4_uint32_t pre = div;

  // post_divider goes up to 15, pre_divider up to 15
  while (pre > 16 && post < 15)
    {
      post++;
      pre = div / (1U << post);
    }

  if (pre > 16)
    pre = 16;
  if (pre < 1)
    pre = 1;

  Conreg con;
  con.raw = read32(CONREG);
  con.pre_divider() = pre - 1;
  con.post_divider() = post;
  write32(CONREG, con.raw);

  trace().printf("Speed config: pre=%u post=%u, actual=%u Hz\n",
                 pre, 1U << post, ref_clk / (pre * (1U << post)));
}

long
Ctrl_imx8::configure(unsigned cs, l4_uint8_t mode, l4_uint32_t speed_hz)
{
  if (cs > 3)
    return -L4_EINVAL;
  if (mode > 3)
    return -L4_EINVAL;

  set_channel_mode(cs, mode);
  set_channel_speed(cs, speed_hz);

  info().printf("Configured CS%u: mode=%u, speed=%u Hz\n", cs, mode, speed_hz);
  return L4_EOK;
}

long
Ctrl_imx8::do_transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                       unsigned len)
{
  if (cs > 3)
    return -L4_EINVAL;

  // Select channel
  Conreg con;
  con.raw = read32(CONREG);
  con.channel_select() = cs;
  con.burst_length() = (len * 8) - 1;
  write32(CONREG, con.raw);

  // Fill TX FIFO (up to 64 words = 256 bytes per burst)
  for (unsigned i = 0; i < len; ++i)
    write32(TXDATA, tx ? tx[i] : 0);

  // Start transfer
  con.raw = read32(CONREG);
  con.xch() = 1;
  write32(CONREG, con.raw);

  // Wait for transfer complete interrupt
  if (wfi())
    {
      error_handler();
      return -L4_EIO;
    }

  // Check TC flag
  Statreg sta;
  sta.raw = read32(STATREG);
  if (!sta.tc())
    {
      warn().printf("Transfer not complete, status: 0x%x\n", sta.raw);
      error_handler();
      return -L4_EIO;
    }

  // Clear TC flag (W1C)
  sta.tc() = 1;
  write32(STATREG, sta.raw);

  // Read RX FIFO
  for (unsigned i = 0; i < len; ++i)
    {
      l4_uint32_t val = read32(RXDATA);
      if (rx)
        rx[i] = val & 0xff;
    }

  return L4_EOK;
}

long
Ctrl_imx8::transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                    unsigned len)
{
  return do_transfer(cs, tx, rx, len);
}

long
Ctrl_imx8::read(unsigned cs, l4_uint8_t *buf, unsigned len)
{
  return do_transfer(cs, nullptr, buf, len);
}

long
Ctrl_imx8::write(unsigned cs, l4_uint8_t const *buf, unsigned len)
{
  return do_transfer(cs, buf, nullptr, len);
}

void
Ctrl_imx8::alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus,
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
Ctrl_imx8::probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu)
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

} // namespace Imx8_spi
