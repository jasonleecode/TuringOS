/*
 * Copyright (C) 2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
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

namespace Imx8
{

static Dbg warn() { return Dbg(Dbg::Warn, "IMX8"); }
static Dbg info() { return Dbg(Dbg::Info, "IMX8"); }
static Dbg trace() { return Dbg(Dbg::Trace, "IMX8"); }


/**
 * Adapter class to manage the I2C controller's MMIO region.
 *
 * The `read` and `write` functions expect a class defining the constants
 * `Offset` and `Write_mask`, as the `Reg_data` class does.
 */
struct Mmio_regs
{
  L4drivers::Mmio_register_block<32> regs;

  Mmio_regs(l4_addr_t base = 0) : regs(base)
  {}

  template <typename T>
  void read(T &reg)
  {
    reg.raw = regs.read<l4_uint16_t>(T::Offset);
  }

  template <typename T>
  void write(T const &reg)
  {
    regs.write<l4_uint16_t>(reg.raw & T::Write_mask, T::Offset);
  }
};

/**
 * Base class for I2C controller register.
 *
 * \tparam OFFS   Offset of the I2C controller register.
 * \tparam WMASK  Bitmask of the writable bits in this register.
 *
 * This class defines the constants to use with `Mmio_regs::read()` and
 * `Mmio_regs::write()`.
 */
template <unsigned OFFS, unsigned WMASK>
struct Reg_data
{
  enum : unsigned { Offset = OFFS, Write_mask = WMASK };

  l4_uint16_t raw = 0;
};

struct Addr_reg : Reg_data<0x0u, 0xfeu>
{
  CXX_BITFIELD_MEMBER( 1, 7, slave_addr, raw);
};

struct Freq_div_reg : Reg_data<0x4u, 0x3fu>
{
  CXX_BITFIELD_MEMBER( 0, 5, ic, raw);
};

struct Control_reg : Reg_data<0x8u, 0xfcu>
{
  CXX_BITFIELD_MEMBER( 7, 7, ien, raw);
  CXX_BITFIELD_MEMBER( 6, 6, iien, raw);
  CXX_BITFIELD_MEMBER( 5, 5, msta, raw);
  CXX_BITFIELD_MEMBER( 4, 4, mtx, raw);
  CXX_BITFIELD_MEMBER( 3, 3, txak, raw);
  CXX_BITFIELD_MEMBER( 2, 2, rsta, raw); // write only

  void print(char const *msg)
  {
    trace().
      printf("%s :: control: %ciin %ciien %cmsta %cmtx %ctxak %crsta (0x%x)\n",
             msg,
             ien()  ? '+' : '-', iien() ? '+' : '-',
             msta()  ? '+' : '-', mtx()  ? '+' : '-',
             txak()  ? '+' : '-', rsta()  ? '+' : '-',
             raw);
  }
};

struct Status_reg : Reg_data<0xcu, 0x12u>
{
  Status_reg() = default;
  Status_reg(Status_reg const &other)
  { raw = other.raw; }

  Status_reg & operator = (Status_reg const &other)
  {
    this->raw = other.raw;
    return *this;
  }

  CXX_BITFIELD_MEMBER_RO( 7, 7, icf, raw);
  CXX_BITFIELD_MEMBER_RO( 6, 6, iaas, raw);
  CXX_BITFIELD_MEMBER_RO( 5, 5, ibb, raw);
  CXX_BITFIELD_MEMBER( 4, 4, ial, raw);
  CXX_BITFIELD_MEMBER_RO( 2, 2, srw, raw);
  CXX_BITFIELD_MEMBER( 1, 1, iif, raw);
  CXX_BITFIELD_MEMBER_RO( 0, 0, rxak, raw);

  void print(char const *msg)
  {
    trace().
      printf("%s :: status: %cicf %ciaas %cibb %cial %csrw %ciif %crxak (0x%x)\n",
             msg,
             icf()  ? '+' : '-', iaas() ? '+' : '-',
             ibb()  ? '+' : '-', ial()  ? '+' : '-',
             srw()  ? '+' : '-', iif()  ? '+' : '-',
             rxak() ? '+' : '-',
             raw);
  }
};

struct Data_reg : Reg_data<0x10u, 0xffu>
{
  CXX_BITFIELD_MEMBER( 0, 7, data, raw);
};


/// i.MX8 I2C controller implementation
class Ctrl_imx8 : public Ctrl_base
{
public:
  Ctrl_imx8() = default;

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

  void handle_irq();

private:
  void alloc_ctrl_resources(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu,
                            L4vbus::Device &dev, l4vbus_device_t &devinfo,
                            l4_addr_t &base, l4_addr_t &end,
                            L4::Cap<L4::Irq> &irq, bool &unmask_at_irq,
                            Dbg const &dbg);

  void clear_status()
  {
    Status_reg sts(_status);
    _status.ial() = 0;
    _status.iif() = 0;
    _mmio_regs.write(_status);
    sts.print("Cleared from");
    _mmio_regs.read(_status);
    _status.print("          to");
  }

  void update_status() { _mmio_regs.read(_status); }
  void update_ctrl()   { _mmio_regs.read(_ctrl); }

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

  /**
   * Wait for some time, if busy-bit is set.
   *
   * \retval false  Busy bit not set.
   * \retval true   Busy bit still set.
   */
  bool wait_not_busy()
  {
    if (_status.ibb())
      l4_sleep(50);

    update_status();

    return _status.ibb();
  }

  void dump_ctrl_state(bool update = false)
  {
    if (update)
      {
        _mmio_regs.read(_addr);
        _mmio_regs.read(_fdr);
        _mmio_regs.read(_ctrl);
        _mmio_regs.read(_status);
      }
    warn().printf("Controller state\n");
    warn().printf("  0x%02x: 0x%04x\n", Addr_reg::Offset, _addr.raw);
    warn().printf("  0x%02x: 0x%04x\n", Freq_div_reg::Offset, _fdr.raw);
    warn().printf("  0x%02x: 0x%04x\n", Control_reg::Offset, _ctrl.raw);
    warn().printf("  0x%02x: 0x%04x\n", Status_reg::Offset, _status.raw);
  }

  /// Reset the controller to a defined state.
  void reset();

  /// Enable the controller.
  void enable_ctrl();

  /// Handle an error ocurred during controller operation.
  void error_handler();

  /// Generate a transaction start signal on the I2C bus.
  void gen_start(bool read, unsigned addr);

  /**
   * Process an ocurred hardware IRQ.
   *
   * \param read          Process the IRQ as read or write operation.
   * \param byte[in,out]  Byte read from the controller or byte to write to it.
   * \param second_last   The `byte` to read is second to last byte to read.
   * \param last          The `byte` to read is the last byte to read.
   *
   * \retval true   IRQ processed.
   * \retval false  Error during IRQ processing.
   */
  bool process_irq(bool read, l4_uint8_t *byte, bool second_last, bool last);


  Mmio_regs _mmio_regs;
  l4_addr_t _end;
  L4::Cap<L4::Irq> _irq;
  bool _unmask_at_irq = false;

  Addr_reg _addr;
  Freq_div_reg _fdr;
  Control_reg _ctrl;
  Status_reg _status;
  Data_reg _data;

  char const *_compatible[2] = {"fsl,imx8mp-i2c", "fsl,imx21-i2c"};
};


void
Ctrl_imx8::setup(L4Re::Util::Object_registry *)
{
  L4Re::chksys(_irq->bind_thread(Pthread::L4::cap(pthread_self()), 0x1d07ca4e),
               "Failed to bind to controller IRQ to thread.");
  L4Re::chkipc(_irq->unmask(), "Failed to unmask controller IRQ.");

  _fdr.ic() = 0x2a;
  _addr.slave_addr() = 0x0;

  _mmio_regs.write(_fdr);
  _mmio_regs.write(_addr);

  reset(); // reset controller to start with a defined state.
}

void Ctrl_imx8::enable_ctrl()
{
  clear_status();

  _ctrl.ien() = 1; // Enable I2C controller
  _ctrl.iien() = 1; // enable interrupts

  _mmio_regs.write(_ctrl);

  _ctrl.print("ENABLE");
}

void Ctrl_imx8::reset()
{
  _ctrl.ien() = 0;
  _ctrl.msta() = 0;
  _ctrl.mtx() = 0;
  _ctrl.txak() = 0;
  _mmio_regs.write(_ctrl);

  enable_ctrl();
}

void Ctrl_imx8::error_handler()
{
  reset();
  clear_status();
}

bool
Ctrl_imx8::process_irq(bool read, l4_uint8_t *byte, bool second_last, bool last)
{
  // Wait until transaction is finished.
  do
    {
      update_status();
      if (!_status.icf())
        {
          trace().printf("Transaction not finished. status.icf not set. "
                         "status 0x%x\n", _status.raw);
          _status.print("");
        }

    } while (!_status.icf());

  update_ctrl();

  if (!_ctrl.msta())
    {
      warn().printf("%s: arbitration lost: status %x\n", __func__, _status.raw);
      return false;
    }

  clear_status();
  if (read)
    {
      assert(!(second_last == last && last == true));

      if (second_last)
        {
          _ctrl.txak() = 1;
          _mmio_regs.write(_ctrl);
        }

      if (last)
        {
          _ctrl.txak() = 0;
          _ctrl.msta() = 0;
          _ctrl.mtx() = 0;
          _mmio_regs.write(_ctrl);
        }

      _mmio_regs.read(_data);
      *byte = _data.data();
    }
  else
    {
      if (_status.rxak())
        {
          warn().printf("%s: rxak set: status %x\n", __func__, _status.raw);
          return false;
        }
      else
        {
          // write more data
          warn().printf("writing 0x%x\n", *byte);
          _data.data() = *byte;
          _mmio_regs.write(_data);
        }
    }

  return true;
}

void
Ctrl_imx8::gen_start(bool read, unsigned addr)
{
  _ctrl.msta() = 1;
  _ctrl.mtx() = 1;
  _mmio_regs.write(_ctrl);

  trace().printf("Waiting for ibb set 0x%x\n", _status.ibb().get());
  for (int i = 0; i < 5 && !_status.ibb(); ++i)
    {
      l4_sleep(50);
      update_status();
    }
  trace().printf("ibb set 0x%x\n", _status.ibb().get());
  if (!_status.ibb())
    return;

  _data.data() = (addr << 1) | read; // LSB == 1 -> Read;
  _mmio_regs.write(_data);
}

long
Ctrl_imx8::read(l4_uint16_t addr, l4_uint8_t *buf, unsigned len)
{
  info().printf("read %u bytes from 0x%x\n", len, addr);

  update_status();
  if (wait_not_busy())
    return -L4_EBUSY;

  gen_start(true, addr);
  if (!_status.ibb())
    {
      error_handler();
      return -L4_EIO;
    }

  if (wfi())
    {
      error_handler();
      return -L4_EIO;
    }

  _ctrl.mtx() = 0;
  _mmio_regs.write(_ctrl);
  update_ctrl();

  if (len == 1)
    {
      // if we just read a single byte, the dummy read is the next-to-last read
      _ctrl.txak() = 1;
      _mmio_regs.write(_ctrl);
    }
  _mmio_regs.read(_data); // dummy read

  _ctrl.print("READ data");
  _status.print("READ data");

  for (unsigned i = 0; i < len; ++i)
    {
      if (wfi())
        {
          error_handler();
          return -L4_EIO;
        }

      if (!process_irq(true, &buf[i], i == (len - 2), i == (len - 1)))
        {
          error_handler();
          return -L4_EINVAL;
        };
    }

  return L4_EOK;
}

long
Ctrl_imx8::write(l4_uint16_t addr, l4_uint8_t const *buf, unsigned len)
{
  info().printf("write %u bytes to 0x%x\n", len, addr);
  update_status();
  _status.print("PRE WRITE");
  if (wait_not_busy())
    return -L4_EBUSY;

  gen_start(false, addr);
  if (!_status.ibb())
    {
      error_handler();
      return -L4_EIO;
    }

  for (unsigned i = 0; i < len; ++i)
    {
      if (wfi())
        {
          error_handler();
          return -L4_EIO;
        }

      l4_uint8_t byte = buf[i];
      if (!process_irq(false, &byte, false, false))
        {
          error_handler();
          return -L4_EINVAL;
        }
    }

  if (wfi())
    {
      error_handler();
      return -L4_EIO;
    }
  update_status();
  clear_status();

  // stop signal
  _ctrl.msta() = 0;
  _ctrl.mtx() = 0;
  _mmio_regs.write(_ctrl);

  return L4_EOK;
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
  _mmio_regs.regs.set_base(base);


  info().printf("MMIO resource [0x%lx, 0x%lx]; IRQ %s\n", base, _end,
                _irq.is_valid() ? "Yes" : "No");

  return true;
}

} // namespace Imx8
