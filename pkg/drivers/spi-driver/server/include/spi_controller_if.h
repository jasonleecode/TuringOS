/*
 * SPI controller hardware abstraction interface.
 */
#pragma once

#include <l4/re/util/object_registry>

namespace Spi_server {

/**
 * Interface for a SPI device to interact with its controller.
 */
struct Controller_if
{
  virtual long transfer(unsigned cs, l4_uint8_t const *tx, l4_uint8_t *rx,
                        unsigned len) = 0;
  virtual long read(unsigned cs, l4_uint8_t *buf, unsigned len) = 0;
  virtual long write(unsigned cs, l4_uint8_t const *buf, unsigned len) = 0;
  virtual long configure(unsigned cs, l4_uint8_t mode,
                         l4_uint32_t speed_hz) = 0;
  virtual void setup(L4Re::Util::Object_registry *registry) = 0;
};

} // namespace Spi_server
