/*
 * BCM2835 SPI0 controller driver.
 *
 * Register layout based on BCM2835 ARM Peripherals document, section 10.
 */
#pragma once

#include <l4/cxx/bitfield>
#include <l4/util/util.h>
#include <l4/drivers/hw_mmio_register_block>

#include "debug.h"
#include "controller.h"
#include "util.h"

#include <thread-l4>

class Ctrl_bcm2835 : public Ctrl_base
{
  // SPI CS register: Control and Status
  struct Cs_reg
  {
    l4_uint32_t raw;

    explicit Cs_reg(Ctrl_bcm2835 *ctrl)
    : raw(ctrl->read32(Mmio_regs::CS) & 0x3fffff)
    {}

    Cs_reg() : raw(0) {}

    CXX_BITFIELD_MEMBER(25, 25, len_long, raw);
    CXX_BITFIELD_MEMBER(24, 24, dma_len, raw);
    CXX_BITFIELD_MEMBER(23, 23, cspol2, raw);
    CXX_BITFIELD_MEMBER(22, 22, cspol1, raw);
    CXX_BITFIELD_MEMBER(21, 21, cspol0, raw);
    CXX_BITFIELD_MEMBER(13, 13, rxf, raw);
    CXX_BITFIELD_MEMBER(12, 12, rxr, raw);
    CXX_BITFIELD_MEMBER(11, 11, txd, raw);
    CXX_BITFIELD_MEMBER(10, 10, rxd, raw);
    CXX_BITFIELD_MEMBER(9, 9, done, raw);
    CXX_BITFIELD_MEMBER(8, 8, te_en, raw);
    CXX_BITFIELD_MEMBER(7, 7, lmono, raw);
    CXX_BITFIELD_MEMBER(6, 6, ren, raw);
    CXX_BITFIELD_MEMBER(5, 5, adcs, raw);
    CXX_BITFIELD_MEMBER(4, 4, intr, raw);
    CXX_BITFIELD_MEMBER(3, 3, intd, raw);
    CXX_BITFIELD_MEMBER(2, 2, dmaen, raw);
    CXX_BITFIELD_MEMBER(1, 1, ta, raw);
    CXX_BITFIELD_MEMBER(0, 0, cpol, raw);
  };

  // FIFO register
  struct Fifo_reg
  {
    l4_uint32_t raw = 0;
    CXX_BITFIELD_MEMBER(0, 7, data, raw);
  };

  // CLK register: clock divider
  struct Clk_reg
  {
    l4_uint32_t raw = 0;
    explicit Clk_reg(l4_uint32_t div) : raw(div) {}
    Clk_reg() = default;
    CXX_BITFIELD_MEMBER(0, 15, cdiv, raw);
  };

  // DLEN register: data length
  struct Dlen_reg
  {
    l4_uint32_t raw = 0;
    explicit Dlen_reg(l4_uint32_t len) : raw(len) {}
    Dlen_reg() = default;
    CXX_BITFIELD_MEMBER(0, 15, len, raw);
  };

  // MMIO register offsets
  enum Mmio_regs
  {
    CS   = 0x00,
    FIFO = 0x04,
    CLK  = 0x08,
    DLEN = 0x0C,
    LTOH = 0x10,
    DC   = 0x14,
  };

public:
  Ctrl_bcm2835() = default;

  bool probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu) override;
  char const *name() override { return _compatible; }

  void setup(L4Re::Util::Object_registry *registry) override;
  long transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                unsigned len) override;
  long read(unsigned cs, l4_uint8_t *buf, unsigned len) override;
  long write(unsigned cs, l4_uint8_t const *buf, unsigned len) override;
  long configure(unsigned cs, l4_uint8_t mode, l4_uint32_t speed_hz) override;

private:
  static Dbg warn() { return Dbg(Dbg::Warn, "BCM2835-SPI"); }
  static Dbg info() { return Dbg(Dbg::Info, "BCM2835-SPI"); }
  static Dbg trace() { return Dbg(Dbg::Trace, "BCM2835-SPI"); }

  l4_uint32_t read32(l4_uint8_t reg)
  { return _regs.read<l4_uint32_t>(reg); }

  void write32(l4_uint8_t reg, l4_uint32_t val)
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

  void select_cs(unsigned cs)
  {
    // CS field is bits [1:0] of the CS register, but we read the whole
    // register and update only the CS bits
    l4_uint32_t val = read32(Mmio_regs::CS);
    val = (val & ~0x3U) | (cs & 0x3U);
    write32(Mmio_regs::CS, val);
  }

  void set_mode(l4_uint8_t mode)
  {
    l4_uint32_t val = read32(Mmio_regs::CS);
    // CPOL is bit 3, CPHA is bit 2 in CS register
    // SPI mode 0: CPOL=0, CPHA=0
    // SPI mode 1: CPOL=0, CPHA=1
    // SPI mode 2: CPOL=1, CPHA=0
    // SPI mode 3: CPOL=1, CPHA=1
    val &= ~((1U << 3) | (1U << 2));
    if (mode & 0x2) // CPOL
      val |= (1U << 3);
    if (mode & 0x1) // CPHA
      val |= (1U << 2);
    write32(Mmio_regs::CS, val);
  }

  void set_speed(l4_uint32_t speed_hz)
  {
    // BCM2835 SPI clock = core_clk / CDIV
    // core_clk is typically 250 MHz on RPi
    // CDIV must be a power of 2, rounded down to nearest even number
    if (speed_hz == 0)
      return;

    l4_uint32_t cdiv = 250000000 / speed_hz;
    // CDIV must be even and at least 2
    cdiv = (cdiv + 1) & ~1U;
    if (cdiv < 2)
      cdiv = 2;
    if (cdiv > 65536)
      cdiv = 0; // 0 means 65536

    write32(Mmio_regs::CLK, cdiv);
  }

  long do_transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                   unsigned len);

  void error_handler()
  {
    // Clear FIFOs and deassert TA
    l4_uint32_t val = read32(Mmio_regs::CS);
    val |= (3U << 4);  // CLEAR bits: clear both FIFOs
    val &= ~(1U << 7); // deassert TA
    write32(Mmio_regs::CS, val);
  }

  void alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu,
                            L4vbus::Device &dev, l4vbus_device_t &devinfo,
                            l4_addr_t &base, l4_addr_t &end,
                            L4::Cap<L4::Irq> &irq, bool &unmask_at_irq,
                            Dbg const &dbg);

  char const *_compatible = "brcm,bcm2835-spi";
  L4drivers::Mmio_register_block<32> _regs;
  l4_addr_t _end;
  L4::Cap<L4::Irq> _irq;
  bool _unmask_at_irq = false;
  l4_uint8_t _mode = 0;
  l4_uint32_t _speed_hz = 1000000;
};

void Ctrl_bcm2835::setup(L4Re::Util::Object_registry *)
{
  L4Re::chksys(_irq->bind_thread(Pthread::L4::cap(pthread_self()), 0x5d10bee),
               "Failed to bind to controller IRQ to thread.");

  // Clear FIFOs and set default state
  l4_uint32_t cs = read32(Mmio_regs::CS);
  cs |= (3U << 4);  // CLEAR: clear both TX and RX FIFOs
  cs |= (1U << 3);  // INTD: interrupt on DONE
  cs &= ~(1U << 7); // TA off initially
  write32(Mmio_regs::CS, cs);

  // Set default clock divider (~1 MHz from 250 MHz core clock)
  set_speed(_speed_hz);

  if (_unmask_at_irq)
    L4Re::chkipc(_irq->unmask(), "Unmask IRQ\n");
}

long
Ctrl_bcm2835::configure(unsigned cs, l4_uint8_t mode, l4_uint32_t speed_hz)
{
  if (cs > 2)
    return -L4_EINVAL;
  if (mode > 3)
    return -L4_EINVAL;

  _mode = mode;
  _speed_hz = speed_hz;

  set_mode(mode);
  set_speed(speed_hz);

  info().printf("Configured CS%u: mode=%u, speed=%u Hz\n", cs, mode, speed_hz);
  return L4_EOK;
}

long
Ctrl_bcm2835::do_transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                          unsigned len)
{
  select_cs(cs);
  set_mode(_mode);

  // Set data length
  write32(Mmio_regs::DLEN, len);

  // Clear FIFOs and start transfer
  l4_uint32_t csval = read32(Mmio_regs::CS);
  csval |= (3U << 4);  // CLEAR FIFOs
  csval |= (1U << 7);  // TA = 1 (Transfer Active)
  csval |= (1U << 3);  // INTD: interrupt on done
  write32(Mmio_regs::CS, csval);

  unsigned tx_count = 0;
  unsigned rx_count = 0;

  while (rx_count < len)
    {
      // Fill TX FIFO
      csval = read32(Mmio_regs::CS);
      while (tx_count < len && (csval & (1U << 18))) // TXD: TX FIFO can accept
        {
          write32(Mmio_regs::FIFO, tx ? tx[tx_count] : 0);
          tx_count++;
          csval = read32(Mmio_regs::CS);
        }

      // Read RX FIFO
      while (rx_count < len && (csval & (1U << 17))) // RXD: RX FIFO has data
        {
          l4_uint8_t byte = read32(Mmio_regs::FIFO) & 0xff;
          if (rx)
            rx[rx_count] = byte;
          rx_count++;
          csval = read32(Mmio_regs::CS);
        }

      // Check DONE
      if (csval & (1U << 16)) // DONE
        {
          // Drain remaining RX FIFO
          while (rx_count < len && (csval & (1U << 17)))
            {
              l4_uint8_t byte = read32(Mmio_regs::FIFO) & 0xff;
              if (rx)
                rx[rx_count] = byte;
              rx_count++;
              csval = read32(Mmio_regs::CS);
            }
          break;
        }

      if (wfi())
        {
          error_handler();
          return -L4_EIO;
        }
    }

  // Deassert TA
  csval = read32(Mmio_regs::CS);
  csval &= ~(1U << 7); // TA = 0
  write32(Mmio_regs::CS, csval);

  return L4_EOK;
}

long
Ctrl_bcm2835::transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                       unsigned len)
{
  if (cs > 2)
    return -L4_EINVAL;

  return do_transfer(cs, tx, rx, len);
}

long
Ctrl_bcm2835::read(unsigned cs, l4_uint8_t *buf, unsigned len)
{
  return do_transfer(cs, nullptr, buf, len);
}

long
Ctrl_bcm2835::write(unsigned cs, l4_uint8_t const *buf, unsigned len)
{
  return do_transfer(cs, buf, nullptr, len);
}

void
Ctrl_bcm2835::alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus,
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
        case L4VBUS_RESOURCE_GPIO:
          {
            enum Gpio_mux
            {
              Fsel0_reg = 0x0,
              Alt_func0 = 0x4, // as defined in bcm2835/bcm2711 spec.
            };

            dbg.printf("Found GPIO resource: [0x%lx, 0x%lx] provider: %li\n",
                       res.start, res.end, res.provider);

            l4_size_t sz = res.end - res.start + 1;
            l4_uint32_t mask = (1 << sz) - 1;

            L4vbus::Gpio_module chipdev(L4vbus::Device(vbus, res.provider));
            L4vbus::Gpio_module::Pin_slice pin_slice(res.start, mask);
            int ret = chipdev.config_pad(pin_slice, Gpio_mux::Fsel0_reg,
                                         Gpio_mux::Alt_func0);
            if (ret != 0)
              dbg.printf("Failed to config GPIO pins: %i. Driver won't work.\n",
                         ret);

            break;
          }
        default:
          dbg.printf("Unhandled resource type %u found: [0x%lx, 0x%lx], "
                     "flags: 0x%x\n",
                     res.type, res.start, res.end, res.flags);
          continue;
        }
    }
}

bool
Ctrl_bcm2835::probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu)
{
  L4vbus::Device dev;
  l4vbus_device_t devinfo;

  if (!find_compatible(_compatible, vbus, dev, devinfo))
    return false;

  info().printf("Found a %s device.\n", _compatible);

  l4_addr_t base = 0UL;
  alloc_ctrl_resources(vbus, icu, dev, devinfo, base, _end, _irq,
                       _unmask_at_irq, info());
  _regs.set_base(base);

  info().printf("MMIO resource [0x%lx, 0x%lx]; IRQ %s\n", base, _end,
         _irq.is_valid() ? "Yes" : "No");

  return true;
}
