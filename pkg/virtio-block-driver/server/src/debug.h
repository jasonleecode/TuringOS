/*
 * Debug utilities for VirtIO block driver
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <cstdio>
#include <l4/libblock-device/debug.h>

namespace Dbg {

enum Level
{
  Error,
  Warn,
  Info,
  Trace,
  Debug
};

static inline void set_level(Level level)
{
  Block_device::Dbg::set_level(static_cast<Block_device::Dbg::Level>(level));
}

static inline Level get_level()
{
  return static_cast<Level>(Block_device::Dbg::get_level());
}

} // namespace Dbg

#endif // DEBUG_H