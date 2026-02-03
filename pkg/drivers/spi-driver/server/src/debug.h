/*
 * SPI driver debug helper (global scope).
 */
#pragma once

#include <l4/re/util/debug>


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

  explicit Dbg(Verbosity v = Warn, char const *subsys = nullptr)
  : L4Re::Util::Dbg(v, "spi-drv", subsys)
  {}
};
