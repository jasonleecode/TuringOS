/*
 * AT24C02 I2C EEPROM driver.
 *
 * The AT24C02 is a 2Kbit (256-byte) serial EEPROM with an I2C interface.
 * Memory is organized as 32 pages of 8 bytes each.
 *
 * I2C address: 0x50 | (A2<<2 | A1<<1 | A0)  — typically 0x50 with all
 * address pins low.
 *
 * Usage:
 *   At24c02 eeprom(i2c_device_cap);
 *   eeprom.write_byte(0x00, 0x42);
 *   l4_uint8_t val;
 *   eeprom.read_byte(0x00, &val);
 */
#pragma once

#include <l4/sys/types.h>
#include <l4/sys/capability>
#include <l4/i2c-driver/i2c_device_if.h>

class At24c02
{
public:
  /** Total storage in bytes. */
  static constexpr unsigned Capacity  = 256;
  /** Page size in bytes (hardware page write boundary). */
  static constexpr unsigned Page_size = 8;
  /** Number of pages. */
  static constexpr unsigned Num_pages = Capacity / Page_size;
  /** Default I2C base address (A2=A1=A0=0). */
  static constexpr l4_uint8_t Base_addr = 0x50;
  /** Write cycle time in microseconds (5 ms per datasheet). */
  static constexpr unsigned Write_cycle_us = 5000;

  /**
   * Construct an AT24C02 driver.
   *
   * \param i2c  I2C device capability returned by the i2c-driver factory
   *             (already bound to the correct 7-bit address).
   */
  explicit At24c02(L4::Cap<I2c_device_ops> i2c);

  /**
   * Read a single byte from the given memory address.
   *
   * \param  addr  EEPROM byte address (0–255).
   * \param  data  Output byte.
   * \retval L4_EOK  Success.
   * \retval <0      Error.
   */
  long read_byte(l4_uint8_t addr, l4_uint8_t *data);

  /**
   * Read \a len bytes starting at \a addr (sequential read).
   *
   * \param  addr  Start address (0–255).
   * \param  buf   Output buffer (must hold at least \a len bytes).
   * \param  len   Number of bytes to read (clamped to Capacity - addr).
   * \retval L4_EOK  Success.
   * \retval <0      Error.
   */
  long read(l4_uint8_t addr, l4_uint8_t *buf, unsigned len);

  /**
   * Write a single byte to the given memory address.
   *
   * Blocks for the device write cycle time (~5 ms) before returning.
   *
   * \param  addr  EEPROM byte address (0–255).
   * \param  data  Byte to write.
   * \retval L4_EOK  Success.
   * \retval <0      Error.
   */
  long write_byte(l4_uint8_t addr, l4_uint8_t data);

  /**
   * Write up to \a len bytes starting at \a addr.
   *
   * Splits the transfer at page boundaries automatically and inserts the
   * required 5 ms write-cycle delay between page writes.
   *
   * \param  addr  Start address (0–255).
   * \param  data  Data to write.
   * \param  len   Number of bytes (clamped to Capacity - addr).
   * \retval L4_EOK  Success.
   * \retval <0      Error on any page write.
   */
  long write(l4_uint8_t addr, l4_uint8_t const *data, unsigned len);

private:
  /**
   * Write up to Page_size bytes that all reside in the same hardware page.
   * The caller must ensure the write stays within one page and waits for
   * the write cycle after this call.
   */
  long write_page(l4_uint8_t addr, l4_uint8_t const *data, unsigned len);

  L4::Cap<I2c_device_ops> _i2c;
};
