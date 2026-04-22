/*
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 */
#pragma once

#include <l4/re/util/debug>

struct Err : L4Re::Util::Err
{
  explicit Err(Level l = Normal) : L4Re::Util::Err(l, "VIRTIO-BLK") {}
};

class Dbg : public L4Re::Util::Dbg
{
public:
  enum Level
  {
    Warn      = 1,
    Info      = 2,
    Trace     = 4,
    Steptrace = 8
  };

  Dbg(unsigned long l = Info, char const *subsys = "")
  : L4Re::Util::Dbg(l, "VIRTIO-BLK", subsys) {}

  static Dbg warn(char const *subsys = "")  { return Dbg(Warn,      subsys); }
  static Dbg info(char const *subsys = "")  { return Dbg(Info,      subsys); }
  static Dbg trace(char const *subsys = "") { return Dbg(Trace,     subsys); }
};

using Err_blockdev = Err;

struct Dbg_blockdev : L4Re::Util::Dbg
{
  Dbg_blockdev(unsigned long l, char const *subsys)
  : L4Re::Util::Dbg(l, "VIRTIO-BLK", subsys) {}
};

#define LIBBLOCKDEV_DEBUG_ERR Err_blockdev
#define LIBBLOCKDEV_DEBUG_DBG Dbg_blockdev
