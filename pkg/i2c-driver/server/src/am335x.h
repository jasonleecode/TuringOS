/*
 * AM335x I2C controller driver.
 *
 * Register layout based on AM335x Technical Reference Manual (SPRUH73),
 * Chapter 21: I2C.
 *
 * The AM335x has three I2C modules:
 *   I2C0: base 0x44E0B000, IRQ 70
 *   I2C1: base 0x4802A000, IRQ 71
 *   I2C2: base 0x4819C000, IRQ 30
 *
 * Each module has a 32-byte TX/RX FIFO.
 * System clock input is typically 48 MHz.
 */
#pragma once

#include <l4/cxx/bitfield>
#include <l4/util/util.h>
#include <l4/drivers/hw_mmio_register_block>

#include "debug.h"
#include "controller.h"
#include "util.h"

#include <thread-l4>

namespace Am335x_i2c {

static Dbg warn() { return Dbg(Dbg::Warn, "AM335x-I2C"); }
static Dbg info() { return Dbg(Dbg::Info, "AM335x-I2C"); }
static Dbg trace() { return Dbg(Dbg::Trace, "AM335x-I2C"); }

// I2C register offsets (OMAP-style, used by AM335x)
enum Regs : unsigned
{
  I2C_REVNB_LO  = 0x00,
  I2C_REVNB_HI  = 0x04,
  I2C_SYSC       = 0x10,
  I2C_IRQSTATUS_RAW = 0x24,
  I2C_IRQSTATUS  = 0x28,
  I2C_IRQENABLE_SET = 0x2C,
  I2C_IRQENABLE_CLR = 0x30,
  I2C_WE         = 0x34,
  I2C_SYSS       = 0x90,
  I2C_BUF        = 0x94,
  I2C_CNT        = 0x98,
  I2C_DATA       = 0x9C,
  I2C_CON        = 0xA4,
  I2C_OA         = 0xA8,
  I2C_SA         = 0xAC,
  I2C_PSC        = 0xB0,
  I2C_SCLL       = 0xB4,
  I2C_SCLH       = 0xB8,
  I2C_SYSTEST    = 0xBC,
  I2C_BUFSTAT    = 0xC0,
};

// I2C_CON bitfields
struct Con_reg
{
  l4_uint32_t raw = 0;

  CXX_BITFIELD_MEMBER(15, 15, en, raw);       // I2C enable
  CXX_BITFIELD_MEMBER(12, 12, opmode, raw);   // HS / F/S mode (reserved on AM335x)
  CXX_BITFIELD_MEMBER(11, 11, stb, raw);      // Start byte mode (HS only)
  CXX_BITFIELD_MEMBER(10, 10, mst, raw);      // Master mode
  CXX_BITFIELD_MEMBER(9, 9, trx, raw);        // TX/RX mode (1=TX, 0=RX)
  CXX_BITFIELD_MEMBER(8, 8, xsa, raw);        // Expand slave address (10-bit)
  CXX_BITFIELD_MEMBER(5, 5, xoa, raw);        // Expand own address (10-bit)
  CXX_BITFIELD_MEMBER(2, 2, stp, raw);        // Stop condition
  CXX_BITFIELD_MEMBER(1, 1, stt, raw);        // Start condition
};

// IRQ bits (I2C_IRQSTATUS / I2C_IRQENABLE)
enum Irq_bits : l4_uint32_t
{
  Irq_xdr  = 1U << 14, // TX data ready
  Irq_rdr  = 1U << 13, // RX data ready
  Irq_bb   = 1U << 12, // Bus busy
  Irq_rovr = 1U << 11, // RX overrun
  Irq_xudf = 1U << 10, // TX underflow
  Irq_aas  = 1U << 9,  // Address as slave
  Irq_bf   = 1U << 8,  // Bus free
  Irq_aerr = 1U << 7,  // Access error
  Irq_stc  = 1U << 6,  // Start condition
  Irq_gc   = 1U << 5,  // General call
  Irq_xrdy = 1U << 4,  // TX data ready (threshold)
  Irq_rrdy = 1U << 3,  // RX data ready (threshold)
  Irq_ardy = 1U << 2,  // Register access ready
  Irq_nack = 1U << 1,  // No ACK
  Irq_al   = 1U << 0,  // Arbitration lost
};


class Ctrl_am335x : public Ctrl_base
{
public:
  Ctrl_am335x() = default;

  bool probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu) override;
  char const *name() override { return _compatible[0]; }

  void setup(L4Re::Util::Object_registry *registry) override;
  long read(l4_uint16_t addr, l4_uint8_t *buf, unsigned len) override;
  long write(l4_uint16_t addr, l4_uint8_t const *buf, unsigned len) override;

  long write_read(l4_uint16_t addr, l4_uint8_t *write_buf, unsigned write_len,
                  l4_uint8_t *read_buf, l4_uint8_t read_len) override
  {
    long ret = write(addr, write_buf, write_len);
    if (ret < 0)
      return ret;
    return read(addr, read_buf, read_len);
  }

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
  void configure_speed();
  bool wait_bus_free();
  void flush_fifos();

  void error_handler()
  {
    // Clear all IRQs
    write32(I2C_IRQSTATUS, 0xFFFF);

    // Disable and re-enable
    soft_reset();
    configure_speed();
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

  // AM335x I2C system clock = 48 MHz, target 100 kHz standard mode
  static constexpr l4_uint32_t Sys_clk = 48000000;
  static constexpr l4_uint32_t I2c_speed = 100000;
  // Internal clock = Sys_clk / (PSC + 1) = 12 MHz with PSC=3
  static constexpr l4_uint32_t Psc_val = 3;
  static constexpr l4_uint32_t Internal_clk = Sys_clk / (Psc_val + 1); // 12 MHz
  // SCLL = SCLH = Internal_clk / (2 * I2c_speed) - 7
  // = 12000000 / 200000 - 7 = 53
  static constexpr l4_uint32_t Scl_val = Internal_clk / (2 * I2c_speed) - 7;

  char const *_compatible[2] = {"ti,omap4-i2c", "ti,omap3-i2c"};
};


void
Ctrl_am335x::soft_reset()
{
  // Disable I2C
  write32(I2C_CON, 0);

  // Soft reset
  write32(I2C_SYSC, 0x2);

  // Wait for reset done (SYSS bit 0)
  for (int i = 0; i < 1000; ++i)
    {
      if (read32(I2C_SYSS) & 0x1)
        break;
      l4_sleep(1);
    }
}

void
Ctrl_am335x::configure_speed()
{
  // Disable I2C before configuring
  write32(I2C_CON, 0);

  // Set prescaler
  write32(I2C_PSC, Psc_val);

  // Set SCL low/high time for ~100 kHz
  write32(I2C_SCLL, Scl_val);
  write32(I2C_SCLH, Scl_val);

  // Set own address to 0
  write32(I2C_OA, 0);

  // Clear all IRQ status
  write32(I2C_IRQSTATUS, 0xFFFF);

  // Enable I2C module in master mode
  Con_reg con;
  con.en() = 1;
  con.mst() = 1;
  write32(I2C_CON, con.raw);

  // Enable relevant interrupts
  write32(I2C_IRQENABLE_SET,
          Irq_xrdy | Irq_rrdy | Irq_ardy | Irq_nack | Irq_al);
}

bool
Ctrl_am335x::wait_bus_free()
{
  for (int i = 0; i < 100; ++i)
    {
      l4_uint32_t stat = read32(I2C_IRQSTATUS_RAW);
      if (!(stat & Irq_bb))
        return true;
      l4_sleep(1);
    }
  warn().printf("Bus busy timeout\n");
  return false;
}

void
Ctrl_am335x::flush_fifos()
{
  l4_uint32_t buf = read32(I2C_BUF);
  buf |= (1U << 14) | (1U << 6); // RXFIFO_CLR | TXFIFO_CLR
  write32(I2C_BUF, buf);
}

void
Ctrl_am335x::setup(L4Re::Util::Object_registry *)
{
  L4Re::chksys(_irq->bind_thread(Pthread::L4::cap(pthread_self()), 0x12ca335),
               "Failed to bind controller IRQ to thread.");

  soft_reset();
  configure_speed();

  if (_unmask_at_irq)
    L4Re::chkipc(_irq->unmask(), "Unmask IRQ\n");
}

long
Ctrl_am335x::write(l4_uint16_t addr, l4_uint8_t const *buf, unsigned len)
{
  info().printf("write %u bytes to 0x%x\n", len, addr);

  if (!wait_bus_free())
    return -L4_EBUSY;

  flush_fifos();
  write32(I2C_IRQSTATUS, 0xFFFF);

  // Set slave address and data count
  write32(I2C_SA, addr);
  write32(I2C_CNT, len);

  // Start TX: master, transmitter, start, stop
  Con_reg con;
  con.en() = 1;
  con.mst() = 1;
  con.trx() = 1;
  con.stt() = 1;
  con.stp() = 1;
  write32(I2C_CON, con.raw);

  unsigned sent = 0;
  while (sent < len)
    {
      if (wfi())
        {
          error_handler();
          return -L4_EIO;
        }

      l4_uint32_t stat = read32(I2C_IRQSTATUS);

      if (stat & Irq_nack)
        {
          warn().printf("NACK received during write to 0x%x\n", addr);
          write32(I2C_IRQSTATUS, Irq_nack);
          error_handler();
          return -L4_EIO;
        }

      if (stat & Irq_al)
        {
          warn().printf("Arbitration lost during write\n");
          write32(I2C_IRQSTATUS, Irq_al);
          error_handler();
          return -L4_EIO;
        }

      if (stat & Irq_xrdy)
        {
          write32(I2C_DATA, buf[sent++]);
          write32(I2C_IRQSTATUS, Irq_xrdy);
        }

      if (stat & Irq_ardy)
        {
          write32(I2C_IRQSTATUS, Irq_ardy);
          break;
        }
    }

  // Wait for bus free
  write32(I2C_IRQSTATUS, 0xFFFF);

  return L4_EOK;
}

long
Ctrl_am335x::read(l4_uint16_t addr, l4_uint8_t *buf, unsigned len)
{
  info().printf("read %u bytes from 0x%x\n", len, addr);

  if (!wait_bus_free())
    return -L4_EBUSY;

  flush_fifos();
  write32(I2C_IRQSTATUS, 0xFFFF);

  // Set slave address and data count
  write32(I2C_SA, addr);
  write32(I2C_CNT, len);

  // Start RX: master, receiver, start, stop
  Con_reg con;
  con.en() = 1;
  con.mst() = 1;
  con.trx() = 0; // receive mode
  con.stt() = 1;
  con.stp() = 1;
  write32(I2C_CON, con.raw);

  unsigned received = 0;
  while (received < len)
    {
      if (wfi())
        {
          error_handler();
          return -L4_EIO;
        }

      l4_uint32_t stat = read32(I2C_IRQSTATUS);

      if (stat & Irq_nack)
        {
          warn().printf("NACK received during read from 0x%x\n", addr);
          write32(I2C_IRQSTATUS, Irq_nack);
          error_handler();
          return -L4_EIO;
        }

      if (stat & Irq_al)
        {
          warn().printf("Arbitration lost during read\n");
          write32(I2C_IRQSTATUS, Irq_al);
          error_handler();
          return -L4_EIO;
        }

      if (stat & Irq_rrdy)
        {
          buf[received++] = read32(I2C_DATA) & 0xFF;
          write32(I2C_IRQSTATUS, Irq_rrdy);
        }

      if (stat & Irq_ardy)
        {
          write32(I2C_IRQSTATUS, Irq_ardy);
          break;
        }
    }

  write32(I2C_IRQSTATUS, 0xFFFF);

  return L4_EOK;
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

} // namespace Am335x_i2c
