/*
 * DS18B20 1-Wire digital temperature sensor driver.
 *
 * The DS18B20 communicates over a single GPIO pin using the Dallas/Maxim
 * 1-Wire protocol.  The line must have an external pull-up resistor
 * (typically 4.7 kΩ) to VCC.  The driver implements open-drain signalling
 * by toggling the GPIO direction between output-low (drive) and input
 * (release, line pulled high by the external resistor).
 *
 * Supported features:
 *   - 9 / 10 / 11 / 12 bit resolution (default 12 bit, 0.0625 °C)
 *   - Single-device "Skip ROM" mode (most common case)
 *   - Multi-device "Match ROM" mode via explicit 64-bit ROM code
 *   - CRC-8 verification of the 9-byte scratchpad
 *   - Parasitic-power compatible (separate V_DD pin recommended)
 *
 * Usage (single device on bus):
 *   L4vbus::Gpio_pin pin(gpio_dev, 4);
 *   Ds18b20 sensor(pin);
 *
 *   int temp_c100;
 *   if (sensor.read_temp_c100(&temp_c100) == L4_EOK)
 *       printf("%.2f °C\n", temp_c100 / 100.0);
 */
#pragma once

#include <l4/sys/types.h>
#include <l4/vbus/vbus_gpio>

class Ds18b20
{
public:
  /** Measurement resolution */
  enum Resolution
  {
    Res_9bit  = 9,   /**<  9-bit,  0.5000 °C, ~94 ms conversion */
    Res_10bit = 10,  /**< 10-bit,  0.2500 °C, ~188 ms conversion */
    Res_11bit = 11,  /**< 11-bit,  0.1250 °C, ~375 ms conversion */
    Res_12bit = 12,  /**< 12-bit,  0.0625 °C, ~750 ms conversion */
  };

  /** 64-bit ROM code (family[7:0] | serial[55:8] | CRC[63:56]) */
  using Rom_code = l4_uint64_t;

  /** Sentinel returned when no ROM code has been set */
  static constexpr Rom_code Skip_rom = 0ULL;

  /**
   * Construct a DS18B20 driver.
   *
   * \param pin  GPIO pin connected to the DS18B20 DQ line.
   *             The pin must have an external 4.7 kΩ pull-up to VCC.
   *             The driver will reconfigure the direction on every bus
   *             transaction.
   */
  explicit Ds18b20(L4vbus::Gpio_pin pin);

  /**
   * Check whether a device responds on the bus (issues a reset).
   *
   * \retval true   Presence pulse detected.
   * \retval false  No device found.
   */
  bool present();

  /**
   * Read the unique 64-bit ROM code.
   *
   * Only works when exactly one device is on the bus.
   *
   * \param[out] rom  ROM code (family byte in bits 7:0).
   * \retval L4_EOK   Success.
   * \retval -L4_EIO  No presence pulse or CRC error.
   */
  long read_rom(Rom_code *rom);

  /**
   * Set the ROM code to use for subsequent operations.
   *
   * Pass \a Skip_rom (0, default) to use the "Skip ROM" command, which
   * addresses all devices simultaneously and is the standard choice when
   * only one DS18B20 is on the bus.
   *
   * \param rom  64-bit ROM code, or \a Skip_rom.
   */
  void set_rom(Rom_code rom) { _rom = rom; }

  /**
   * Configure the measurement resolution.
   *
   * Writes the configuration register in the DS18B20's scratchpad and
   * copies it to EEPROM.
   *
   * \param res  Desired resolution (9–12 bit).
   * \retval L4_EOK   Success.
   * \retval -L4_EIO  No presence or CRC mismatch.
   */
  long set_resolution(Resolution res);

  /**
   * Trigger a temperature conversion and read the result.
   *
   * Blocks for the conversion time (up to 750 ms at 12-bit resolution).
   * Reads back and CRC-verifies the full 9-byte scratchpad.
   *
   * \param[out] temp_c100  Temperature in units of 1/100 °C.
   *                        E.g. 2550 → 25.50 °C, -1025 → -10.25 °C.
   * \retval L4_EOK   Success.
   * \retval -L4_EIO  No presence pulse, bus error, or CRC mismatch.
   */
  long read_temp_c100(int *temp_c100);

  /**
   * Read the raw 16-bit scratchpad temperature word.
   *
   * Useful when the caller wants full resolution without the fixed-point
   * conversion.  Raw value × 625 = temperature in units of 1/10000 °C.
   *
   * \param[out] raw  Raw 16-bit temperature register value.
   * \retval L4_EOK   Success.
   * \retval -L4_EIO  Bus error or CRC mismatch.
   */
  long read_temp_raw(l4_int16_t *raw);

private:
  /* ---- 1-Wire bus primitives ---- */

  /** Drive DQ low (output, 0). */
  void pin_low();
  /** Release DQ (input, high via pull-up). */
  void pin_release();
  /** Read DQ state (must call pin_release() first). */
  int  pin_read();

  /**
   * Issue a 1-Wire reset and check for presence pulse.
   * \retval true   Presence detected.
   * \retval false  No presence (line stayed high).
   */
  bool reset();

  /** Write one bit (1-Wire time slot). */
  void write_bit(bool b);
  /** Read one bit (1-Wire time slot). */
  bool read_bit();

  /** Write an 8-bit byte LSB-first. */
  void write_byte(l4_uint8_t byte);
  /** Read an 8-bit byte LSB-first. */
  l4_uint8_t read_byte();

  /**
   * Issue reset + ROM command (Skip ROM or Match ROM) to address the device.
   * \retval true   Presence detected.
   * \retval false  No presence.
   */
  bool select();

  /** Read full 9-byte scratchpad into \a buf. */
  long read_scratchpad(l4_uint8_t buf[9]);

  /* ---- Utilities ---- */

  /** Busy-wait for \a us microseconds using the KIP clock. */
  static void delay_us(unsigned us);

  /** Dallas/Maxim CRC-8 (poly 0x31) over \a len bytes. */
  static l4_uint8_t crc8(l4_uint8_t const *data, unsigned len);

  L4vbus::Gpio_pin _pin;
  Rom_code         _rom = Skip_rom;
};
