/*
 * Copyright (C) 2024-2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/re/util/object_registry>

namespace I2c_server {

/**
 * Interface for a i2c device to interact with its controller.
 */
struct Controller_if
{
  virtual long read(l4_uint16_t addr, l4_uint8_t *buf, unsigned len) = 0;
  virtual long write(l4_uint16_t addr, l4_uint8_t const *vals, unsigned len) = 0;
  virtual long write_read(l4_uint16_t addr,
                          l4_uint8_t *write_buf, unsigned write_len,
                          l4_uint8_t *read_buf, l4_uint8_t read_len) = 0;
  virtual void setup(L4Re::Util::Object_registry *registry) = 0;
};

} // namespace I2c_server
