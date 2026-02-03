/*
 * Copyright (C) 2024-2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
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
  : L4Re::Util::Dbg(v, "i2c-drv", subsys)
  {}
};

