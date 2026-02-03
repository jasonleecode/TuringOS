/*
 * SPI server debug helper.
 */
#pragma once

#include <l4/re/util/debug>

namespace Spi_server {

class Dbg : public L4Re::Util::Dbg
{
public:
  enum Verbosity : unsigned long
  {
    Quiet = 0,
    Warn = 1,
    Info = 2,
    Trace = 4,
  };

  explicit Dbg(Verbosity v = Warn, char const *subsys = "")
  : L4Re::Util::Dbg(v, "spi-drv", subsys)
  {}
};

} // namespace Spi_server
