/*
 * SPI server implementation: Factory, Device, VirtIO.
 */

#include <l4/sys/factory>
#include <l4/sys/cxx/ipc_types>
#include <l4/sys/cxx/ipc_epiface>
#include <l4/re/error_helper>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>

#include <l4/spi-driver/spi_device_if.h>

#include <l4/spi-driver/server/spi_server.h>
#include <l4/spi-driver/server/spi_controller_if.h>

#include <pthread-l4.h>
#include <memory>
#include <tuple>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <string>
#include <cassert>

#include "debug.h"

namespace Spi_server {

L4Re::Util::Err err(L4Re::Util::Err::Normal, "SPI Server");

static bool
parse_uint_optstring(char const *optstring, unsigned int *out)
{
  char *endp;

  errno = 0;
  unsigned long num = strtoul(optstring, &endp, 0);

  // check that long can be converted to int
  if (errno || *endp != '\0' || num > UINT_MAX)
    return false;

  *out = num;

  return true;
}

static bool
parse_uint_param(L4::Ipc::Varg const &param, char const *prefix,
                 unsigned int *out)
{
  l4_size_t headlen = strlen(prefix);

  if (param.length() < headlen)
    return false;

  char const *pstr = param.value<char const *>();

  if (strncmp(pstr, prefix, headlen) != 0)
    return false;

  std::string tail(pstr + headlen, param.length() - headlen);

  if (!parse_uint_optstring(tail.c_str(), out))
    {
      err.printf("Bad parameter '%s'. Invalid number specified.\n", prefix);
      throw L4::Runtime_error(-L4_EINVAL);
    }

  return true;
}

class Spi_device : public L4::Epiface_t<Spi_device, Spi_device_ops>
{
public:
  Spi_device(unsigned cs, l4_uint8_t mode, l4_uint32_t speed_hz,
             Controller_if *ctrl)
  : _ctrl(ctrl), _cs(cs), _mode(mode), _speed_hz(speed_hz)
  {
    assert(_ctrl);
  }

  long op_transfer(Spi_device_ops::Rights,
                   L4::Ipc::Array_ref<l4_uint8_t const> tx_buf,
                   unsigned short rx_len,
                   L4::Ipc::Array_ref<l4_uint8_t> &rx_buf);
  long op_read(Spi_device_ops::Rights, unsigned short len,
               L4::Ipc::Array_ref<l4_uint8_t> &buf);
  long op_write(Spi_device_ops::Rights,
                L4::Ipc::Array_ref<l4_uint8_t const> buf);
  long op_configure(Spi_device_ops::Rights, l4_uint8_t mode,
                    l4_uint32_t speed_hz);

  bool match(unsigned cs) const { return cs == _cs; }

private:
  Controller_if *_ctrl;
  unsigned _cs;
  l4_uint8_t _mode;
  l4_uint32_t _speed_hz;
};

long
Spi_device::op_transfer(Spi_device_ops::Rights,
                        L4::Ipc::Array_ref<l4_uint8_t const> tx_buf,
                        unsigned short rx_len,
                        L4::Ipc::Array_ref<l4_uint8_t> &rx_buf)
{
  unsigned len = tx_buf.length > rx_len ? tx_buf.length : rx_len;
  std::vector<l4_uint8_t> tx(len, 0);
  std::vector<l4_uint8_t> rx(len, 0);
  memcpy(tx.data(), tx_buf.data, tx_buf.length);

  long ret = _ctrl->transfer(_cs, tx.data(), rx.data(), len);
  if (ret >= 0)
    {
      memcpy(rx_buf.data, rx.data(), rx_len);
      return L4_EOK;
    }
  return ret;
}

long
Spi_device::op_read(Spi_device_ops::Rights, unsigned short len,
                    L4::Ipc::Array_ref<l4_uint8_t> &buf)
{
  std::vector<l4_uint8_t> buffer(len);
  long ret = _ctrl->read(_cs, buffer.data(), len);

  if (ret >= 0)
    {
      memcpy(buf.data, buffer.data(), len);
      return L4_EOK;
    }
  return ret;
}

long
Spi_device::op_write(Spi_device_ops::Rights,
                     L4::Ipc::Array_ref<l4_uint8_t const> buf)
{
  std::vector<l4_uint8_t> buffer(buf.data, buf.data + buf.length);
  return _ctrl->write(_cs, buffer.data(), buf.length);
}

long
Spi_device::op_configure(Spi_device_ops::Rights, l4_uint8_t mode,
                         l4_uint32_t speed_hz)
{
  if (mode > 3)
    return -L4_EINVAL;

  long ret = _ctrl->configure(_cs, mode, speed_hz);
  if (ret >= 0)
    {
      _mode = mode;
      _speed_hz = speed_hz;
    }
  return ret;
}

class Spi_factory : public L4::Epiface_t<Spi_factory, L4::Factory>
{
  enum Device_type
  {
    Type_virtio = 0,
    Type_rpc = 1,
  };

  struct Del_irq : L4::Irqep_t<Del_irq>
  {
    explicit Del_irq(Spi_factory *p)
    : _p{p} {}

    void handle_irq()
    { _p->check_clients(); }

  private:
    Spi_factory *_p;
  };

public:
  Spi_factory(L4Re::Util::Object_registry *registry, Controller_if *ctrl)
  : _ctrl(ctrl), _registry(registry), _del_irq(this)
  {
    auto c = L4Re::chkcap(_registry->register_irq_obj(&_del_irq));
    L4Re::chksys(Pthread::L4::cap(pthread_self())->register_del_irq(c));
  }

  ~Spi_factory()
  {
    _registry->unregister_obj(&_del_irq);
  }

  /**
   * Create a SPI device connection.
   *
   * \param[out] res   Capability to access device.
   * \param      type  Object protocol: [0, 1]
   * \param      args  Arguments: cs=N, mode=N, speed=N
   */
  long op_create(L4::Factory::Rights, L4::Ipc::Cap<void> &res,
                 l4_umword_t type, L4::Ipc::Varg_list<> &&args);

private:
  static Dbg warn() { return Dbg(Dbg::Warn, "Fab"); }
  static Dbg trace() { return Dbg(Dbg::Trace, "Fab"); }

  bool cs_free(unsigned cs) const;

  void check_clients()
  {
    auto it = _devices_rpc.begin();
    while (it != _devices_rpc.end())
      {
        if ((*it)->obj_cap().validate().label() == 0)
          it = _devices_rpc.erase(it);
        else
          ++it;
      }
  }

  Controller_if *_ctrl;
  L4Re::Util::Object_registry *_registry;
  Del_irq _del_irq;
  std::vector<std::unique_ptr<Spi_device>> _devices_rpc;
};

bool
Spi_factory::cs_free(unsigned cs) const
{
  for (auto const &dev : _devices_rpc)
    if (dev->match(cs))
      return false;

  return true;
}

long
Spi_factory::op_create(L4::Factory::Rights, L4::Ipc::Cap<void> &res,
                       l4_umword_t type, L4::Ipc::Varg_list<> &&args)
{
  warn().printf("Received create request for type %lu\n", type);
  if (type > 1)
    return -L4_ENODEV;

  unsigned int cs = 0;
  unsigned int mode = 0;
  unsigned int speed = 1000000; // default 1 MHz

  for (L4::Ipc::Varg const &arg : args)
    {
      if (!arg.is_of<char const *>())
        {
          err.printf("Unexpected type for argument\n");
          return -L4_EINVAL;
        }

      try
        {
          if (parse_uint_param(arg, "cs=", &cs))
            continue;
          if (parse_uint_param(arg, "mode=", &mode))
            {
              if (mode > 3)
                {
                  err.printf("Requested SPI mode %u exceeds valid range 0-3\n",
                             mode);
                  return -L4_EINVAL;
                }
              continue;
            }
          if (parse_uint_param(arg, "speed=", &speed))
            continue;
        }
      catch (L4::Runtime_error &e)
        {
          return e.err_no();
        }
    }

  warn().printf("Create request for CS %u, mode %u, speed %u\n", cs, mode,
                speed);

  if (!cs_free(cs))
    return -L4_EEXIST;

  // Configure the controller for this CS
  long ret = _ctrl->configure(cs, mode, speed);
  if (ret < 0)
    return ret;

  L4::Cap<void> device_ep;

  switch (type)
    {
    case Type_rpc:
      {
        std::unique_ptr<Spi_device> spi_dev =
          std::make_unique<Spi_device>(cs, mode, speed, _ctrl);

        if (!spi_dev)
          return -L4_EINVAL;

        device_ep = _registry->register_obj(spi_dev.get());
        if (!device_ep)
          return -L4_EINVAL;

        trace().printf("Created SPI device for CS %u: %p\n", cs,
                       spi_dev.get());

        _devices_rpc.push_back(std::move(spi_dev));
        break;
      }

    case Type_virtio:
      // VirtIO SPI not yet supported
      return -L4_ENODEV;

    default: return -L4_ENODEV;
    }

  L4::cap_cast<L4::Kobject>(device_ep)->dec_refcnt(1);
  res = L4::Ipc::make_cap_rw(device_ep);

  return L4_EOK;
}

static std::tuple<L4::Cap<void>, std::unique_ptr<Spi_factory>>
init_factory(L4Re::Util::Object_registry *registry, char const *cap_name,
             Controller_if *ctrl)
{
  Dbg warn(Dbg::Warn, "Fab");
  Dbg trace(Dbg::Trace, "Fab");

  std::unique_ptr<Spi_factory> factory =
    std::make_unique<Spi_factory>(registry, ctrl);

  L4::Cap<void> cap = registry->register_obj(factory.get(), cap_name);

  if (!cap)
    {
      warn.printf("Capability %s for factory interface not in capability table. "
                  "No SPI devices can be connected.\n", cap_name);
      return std::make_tuple(cap, nullptr);
    }
  else
    trace.printf("factory cap 0x%lx\n", factory->obj_cap().cap());

  return std::make_tuple(cap, std::move(factory));
}

void start_server(Controller_if *ctrl)
{
  using Reg_Server =
    L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks>;

  Dbg warn(Dbg::Warn, "Srv");
  Dbg trace(Dbg::Trace, "Srv");

  // use myself as server thread.
  auto server = std::make_shared<Reg_Server>(Pthread::L4::cap(pthread_self()),
                                             L4Re::Env::env()->factory());

  ctrl->setup(server->registry());

  auto [factory_cap, factory] =
    init_factory(server->registry(), "dev_factory", ctrl);

  if (factory != nullptr)
    {
      trace.printf("factory initialized. start server loop\n");
      // start server loop to allow for factory connections.
      server->loop();

      warn.printf("exited server loop. Cleaning up ...\n");
      server->registry()->unregister_obj(factory.get());
      factory_cap.invalidate();
    }
  else
    warn.printf("Factory registration failed.\n");;
}

} // namespace Spi_server
