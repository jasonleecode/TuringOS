/*
 * Copyright (C) 2024-2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
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
  struct Control
  {
    l4_uint32_t raw;

    explicit Control(Ctrl_bcm2835 *ctrl)
    : raw(ctrl->read32(Mmio_regs::C) & 0x87b1U)
    {}

    CXX_BITFIELD_MEMBER(15, 15, i2c_en, raw);
    CXX_BITFIELD_MEMBER(10, 10, intr, raw);
    CXX_BITFIELD_MEMBER(9, 9, intt, raw);
    CXX_BITFIELD_MEMBER(8, 8, intd, raw);
    CXX_BITFIELD_MEMBER(7, 7, st, raw);
    CXX_BITFIELD_MEMBER(4, 5, clear, raw);
    CXX_BITFIELD_MEMBER(0, 0, read, raw);
  };

  struct Status
  {
    l4_uint32_t raw;

    explicit Status(Ctrl_bcm2835 *ctrl) { update(ctrl); }

    void update(Ctrl_bcm2835 *ctrl)
    { raw = ctrl->read32(Mmio_regs::S) & 0x3ffU; }

    CXX_BITFIELD_MEMBER(9, 9, clkt, raw);
    CXX_BITFIELD_MEMBER(8, 8, err, raw);
    CXX_BITFIELD_MEMBER(7, 7, rxf, raw);
    CXX_BITFIELD_MEMBER(6, 6, txe, raw);
    CXX_BITFIELD_MEMBER(5, 5, rxd, raw);
    CXX_BITFIELD_MEMBER(4, 4, txd, raw);
    CXX_BITFIELD_MEMBER(3, 3, rxr, raw);
    CXX_BITFIELD_MEMBER(2, 2, txw, raw);
    CXX_BITFIELD_MEMBER(1, 1, done, raw);
    CXX_BITFIELD_MEMBER(0, 0, ta, raw);
  };

  struct Reg_lower_16
  {
    l4_uint32_t raw = 0;
    Reg_lower_16() = default;
    explicit Reg_lower_16(l4_uint32_t data) : raw(data) {}

    CXX_BITFIELD_MEMBER(0, 15, data, raw);
  };
  using Clock_div = Reg_lower_16;
  using Clock_tout = Reg_lower_16;

  struct Data_len : Reg_lower_16
  {
    using Reg_lower_16::Reg_lower_16;
    explicit Data_len(Ctrl_bcm2835 *ctrl) { update(ctrl); }

    void update(Ctrl_bcm2835 *ctrl)
    { raw = ctrl->read32(Mmio_regs::Dlen) & 0xffff; }
  };

  struct Slave_addr
  {
    l4_uint32_t raw = 0;
    explicit Slave_addr(l4_uint32_t addr) : raw(addr) {}

    CXX_BITFIELD_MEMBER(0, 6, addr, raw);
  };

  struct Fifo
  {
    l4_uint32_t raw = 0;
    CXX_BITFIELD_MEMBER(0, 7, data, raw);
  };

  struct Del
  {
    l4_uint32_t raw = 0;
    CXX_BITFIELD_MEMBER(16, 31, fedl, raw);
    CXX_BITFIELD_MEMBER(0, 15, redl, raw);
  };

  // Offsets of the MMIO registers
  enum Mmio_regs
  {
    C = 0x0,
    S = 0x4,
    Dlen = 0x8,
    A = 0xc,
    Fifo = 0x10,
    Cdiv = 0x14,
    Del = 0x18,
    Clkt = 0x1c,
  };

public:
  Ctrl_bcm2835() = default;

  bool probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu) override;
  char const *name() override { return _compatible; }

  void setup(L4Re::Util::Object_registry *registry) override;
  long read(l4_uint16_t addr, l4_uint8_t *buf, unsigned len) override;
  long write(l4_uint16_t addr, l4_uint8_t const *vals, unsigned len) override;

  long write_read(l4_uint16_t addr, l4_uint8_t *write_buf, unsigned write_len,
                  l4_uint8_t *read_buf, l4_uint8_t read_len) override
  {
    long ret = write(addr, write_buf, write_len);
    if (ret < 0)
      return ret;
    return read(addr, read_buf, read_len);
  }

private:
  static Dbg warn() { return Dbg(Dbg::Warn, "BCM2835"); }
  static Dbg info() { return Dbg(Dbg::Info, "BCM2835"); }
  static Dbg trace() { return Dbg(Dbg::Trace, "BCM2835"); }

  l4_uint32_t read32(l4_uint8_t reg)
  {
//    trace().printf("read reg 0x%x\n", reg);
    return _regs.read<l4_uint32_t>(reg);
  }

  void write32(l4_uint8_t reg, l4_uint32_t val)
  {
//    trace().printf("write 0x%x to reg 0x%x\n", val, reg);
    _regs.write<l4_uint32_t>(val, reg);
  }

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

  long process_irq_read(l4_uint8_t *buf, unsigned buflen, unsigned &byte_count);
  long process_irq_write(l4_uint8_t const *buf, unsigned buflen,
                         unsigned &byte_count);

  bool has_status_error(Status s) const;
  void start_transfer(l4_uint16_t addr, l4_uint16_t len, bool read, bool clear);
  void finish_transfer();

  void error_handler()
  {
    Status s(this);
    s.done() = 1;
    s.err() = 1;
    s.clkt() = 1;
    write32(Mmio_regs::S, s.raw);

    Control c(this);
    c.clear();
    write32(Mmio_regs::C, c.raw);
  }

  void dump_ctrl_state()
  {
    // NOTE: This is a destructive read for the fifo register.
    warn().printf("Register: Value\n");
    for (unsigned reg = 0; reg <= Mmio_regs::Clkt; reg += 4)
      warn().printf("0x%02x: 0x%04x\n", reg, read32(reg));
  }

  void alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu,
                            L4vbus::Device &dev, l4vbus_device_t &devinfo,
                            l4_addr_t &base, l4_addr_t &end,
                            L4::Cap<L4::Irq> &irq, bool &unmask_at_irq,
                            Dbg const &dbg);

  char const *_compatible = "brcm,bcm2835-i2c";
  L4drivers::Mmio_register_block<32> _regs;
  l4_addr_t _end;
  L4::Cap<L4::Irq> _irq;
  bool _unmask_at_irq = false;
};

void Ctrl_bcm2835::setup(L4Re::Util::Object_registry *)
{
  L4Re::chksys(_irq->bind_thread(Pthread::L4::cap(pthread_self()), 0xc00ffee),
               "Failed to bind to controller IRQ to thread.");

  Control c(this);
  c.i2c_en() = 1;
  c.intr() = 1;  // enable interrupt on RXR
  c.intt() = 1;  // enable interrupt on TXW
  c.intd() = 1;  // enable interrupt on DONE
  c.clear() = 1;
  write32(Mmio_regs::C, c.raw);

  // TODO should we use this? linux sets it to zero/disabled.
  write32(Mmio_regs::Clkt, 0U);

  if (_unmask_at_irq)
    L4Re::chkipc(_irq->unmask(), "Unmask IRQ\n");
}

bool
Ctrl_bcm2835::has_status_error(Status s) const
{
  if (s.err() || s.clkt())
    {
      info().printf("Slave error: %s (status 0x%x)\n",
                    s.err() ? "address not ack'd"
                            : (s.clkt() ? "clock strech timeout" : "none"),
                    s.raw);
      return true;
    }

  return false;
}


long
Ctrl_bcm2835::process_irq_read(l4_uint8_t *buf, unsigned buflen,
                               unsigned &byte_count)
{
  Status s(this);
  if (has_status_error(s))
    {
      error_handler();
      return -L4_EIO;
    }

  if (s.done())
    {
      while (byte_count < buflen && s.rxd())
        {
          buf[byte_count++] = read32(Mmio_regs::Fifo);
          s.update(this);
        }
      if (byte_count == buflen)
        return L4_EOK;

      printf("More data expected (cnt: %u, buflen %u), but FIFO empty: "
             "status 0x%x\n",
             byte_count, buflen, s.raw);
      return -L4_EIO;
    }
  else if (s.rxr())
    {
      // read fifo
      while (byte_count < buflen && s.rxd())
        {
          buf[byte_count++] = read32(Mmio_regs::Fifo);
          s.update(this);
        }

      return -L4_EAGAIN;
    }
  else
    {
      printf("unknown interrupt reason during read. Ignoring. 0x%x\n", s.raw);
      return -L4_EAGAIN;
    }
}

long
Ctrl_bcm2835::read(l4_uint16_t addr, l4_uint8_t *buf, unsigned len)
{
  start_transfer(addr, len, true, true);

  unsigned processed_bytes = 0;
  while (true)
    {
      if (wfi())
        {
          error_handler();
          return -L4_EIO;
        }

      long ret = process_irq_read(buf, len, processed_bytes);
      if (ret != -L4_EAGAIN)
        {
          finish_transfer();
          return ret;
        }
    }
}

long
Ctrl_bcm2835::process_irq_write(l4_uint8_t const *buf, unsigned buflen,
                                unsigned &byte_count)
{
  Status s(this);
  if (has_status_error(s))
    {
      error_handler();
      return -L4_EIO;
    }

  if (s.done())
    return L4_EOK;
  else if(s.txw())
    {
      // fill fifo
      while (byte_count < buflen && s.txd())
        {
          write32(Mmio_regs::Fifo, buf[byte_count++]);
          s.update(this);
        }

      return -L4_EAGAIN;
    }
  else
    {
      printf("unknown interrupt reason during write. Ignoring. 0x%x\n", s.raw);
      return -L4_EAGAIN;
    }
}

long
Ctrl_bcm2835::write(l4_uint16_t addr, l4_uint8_t const *vals, unsigned len)
{
  start_transfer(addr, len, false, true);

  unsigned processed_bytes = 0;
  while (true)
    {
      if (wfi())
        {
          error_handler();
          return -L4_EIO;
        }

      long ret = process_irq_write(vals, len, processed_bytes);

      if (ret != -L4_EAGAIN)
        {
          finish_transfer();
          return ret;
        }
    }
}

void
Ctrl_bcm2835::start_transfer(l4_uint16_t addr, l4_uint16_t len, bool read,
                             bool clear)
{
  Slave_addr slave(addr);
  write32(Mmio_regs::A, slave.addr());

  Data_len dlen(len);
  write32(Mmio_regs::Dlen, dlen.data());

  Control c(this);
  c.st() = 1;
  c.clear() = clear;
  c.read() = read;
  write32(Mmio_regs::C, c.raw);
}

void
Ctrl_bcm2835::finish_transfer()
{
  Status s(this);
  if (s.err())
    info().printf("Slave address error flagged. Status 0x%x\n", s.raw);
  if (s.clkt())
    info().printf("Slave strechted clock timeout too far. Status: 0x%x\n",
                  s.raw);

  s.done() = 1;
  s.err() = 1;
  s.clkt() = 1;
  write32(Mmio_regs::S, s.raw);
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
