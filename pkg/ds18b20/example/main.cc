/*
 * DS18B20 temperature sensor demonstration.
 *
 * Required capabilities in the ned environment:
 *   vbus  – virtual bus containing a GPIO device whose pin is connected
 *           to the DS18B20 DQ line (with 4.7 kΩ pull-up to VCC).
 *
 * Kconfig knobs (with defaults):
 *   CONFIG_DS18B20_GPIO_PIN   – GPIO pin number (default 4)
 *   CONFIG_DS18B20_SAMPLES    – number of readings to take (default 5)
 *
 * The example:
 *   1. Scans the bus for a device presence pulse.
 *   2. Reads the 64-bit ROM code (family / serial / CRC).
 *   3. Takes CONFIG_DS18B20_SAMPLES temperature readings and prints them.
 */

#include <l4/ds18b20/ds18b20.h>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_gpio>
#include <l4/util/util.h>

#include <cstdio>

#ifndef CONFIG_DS18B20_GPIO_PIN
#  define CONFIG_DS18B20_GPIO_PIN   4
#endif

#ifndef CONFIG_DS18B20_SAMPLES
#  define CONFIG_DS18B20_SAMPLES    5
#endif

int main()
{
  printf("DS18B20 1-Wire temperature sensor example\n");
  printf("  GPIO pin : %d\n", CONFIG_DS18B20_GPIO_PIN);
  printf("  Samples  : %d\n", CONFIG_DS18B20_SAMPLES);

  /* ---- Obtain vbus and locate GPIO device ---- */
  auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (!vbus)
    {
      printf("Error: 'vbus' capability not found\n");
      return 1;
    }

  L4vbus::Device gpio_dev;
  if (vbus->root().device_by_hid(&gpio_dev, "gpio") < 0)
    {
      printf("Error: GPIO device not found on vbus\n");
      return 1;
    }

  L4vbus::Gpio_pin dq(gpio_dev, CONFIG_DS18B20_GPIO_PIN);

  /* ---- Construct driver ---- */
  Ds18b20 sensor(dq);

  /* ---- Step 1: Check presence ---- */
  printf("\n[Step 1] Bus scan\n");
  if (!sensor.present())
    {
      printf("  No device detected on pin %d\n", CONFIG_DS18B20_GPIO_PIN);
      printf("  Check wiring and pull-up resistor.\n");
      return 1;
    }
  printf("  Device present.\n");

  /* ---- Step 2: Read ROM code ---- */
  printf("\n[Step 2] Read ROM code\n");
  Ds18b20::Rom_code rom = Ds18b20::Skip_rom;
  if (sensor.read_rom(&rom) == L4_EOK)
    {
      l4_uint8_t family  = rom & 0xFF;
      l4_uint64_t serial = (rom >> 8) & 0xFFFFFFFFFFFFULL;
      l4_uint8_t crc     = (rom >> 56) & 0xFF;

      printf("  Family  : 0x%02x%s\n", family,
             family == 0x28 ? " (DS18B20)" : " (unexpected)");
      printf("  Serial  : %012llx\n", (unsigned long long)serial);
      printf("  CRC     : 0x%02x\n", crc);

      /* Use this ROM for subsequent reads (enables multi-device setups) */
      sensor.set_rom(rom);
    }
  else
    {
      printf("  ROM read failed (check for multiple devices on bus).\n");
      printf("  Continuing with Skip ROM.\n");
    }

  /* ---- Step 3: Temperature readings ---- */
  printf("\n[Step 3] Temperature readings (%d samples, 12-bit resolution)\n",
         CONFIG_DS18B20_SAMPLES);

  int errors = 0;
  for (int i = 0; i < CONFIG_DS18B20_SAMPLES; ++i)
    {
      int temp_c100;
      long r = sensor.read_temp_c100(&temp_c100);
      if (r < 0)
        {
          printf("  Sample %d: ERROR (%ld)\n", i + 1, r);
          ++errors;
          continue;
        }

      /* Format as signed decimal with two fractional digits */
      int sign    = (temp_c100 < 0) ? -1 : 1;
      int abs_val = (temp_c100 < 0) ? -temp_c100 : temp_c100;
      int whole   = abs_val / 100;
      int frac    = abs_val % 100;

      printf("  Sample %d: %s%d.%02d °C  (raw/100 = %d)\n",
             i + 1,
             (sign < 0) ? "-" : "",
             whole, frac,
             temp_c100);

      /* Small pause between samples */
      if (i + 1 < CONFIG_DS18B20_SAMPLES)
        l4_sleep(500);
    }

  if (errors == 0)
    printf("\nAll %d samples read successfully.\n", CONFIG_DS18B20_SAMPLES);
  else
    printf("\n%d/%d samples failed.\n", errors, CONFIG_DS18B20_SAMPLES);

  return errors ? 1 : 0;
}
