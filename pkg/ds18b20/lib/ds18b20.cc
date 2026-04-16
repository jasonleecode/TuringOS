/*
 * DS18B20 1-Wire temperature sensor driver implementation.
 *
 * 1-Wire timing (from Maxim/Microchip DS18B20 datasheet, Table 1):
 *
 *  Reset / Presence:
 *    A  Master drives low ≥ 480 µs
 *    B  Master releases, waits 15–60 µs
 *    C  Device pulls low 60–240 µs (presence pulse)
 *    D  Master samples at ~30 µs after releasing, then waits for total ≥ 480 µs
 *
 *  Write-1 slot (60–120 µs total, ≥ 1 µs recovery):
 *    A  Drive low 1–15 µs  (we use 6 µs)
 *    B  Release for remainder of 60 µs slot
 *
 *  Write-0 slot (60–120 µs total, ≥ 1 µs recovery):
 *    A  Drive low 60–120 µs (we use 60 µs)
 *    B  Release, 2 µs recovery
 *
 *  Read slot (≥ 60 µs total, ≥ 1 µs recovery):
 *    A  Drive low ≥ 1 µs (we use 3 µs)
 *    B  Release, sample within 15 µs of slot start (we sample at ~10 µs)
 *    C  Remainder of 60 µs slot
 *
 * Open-drain signalling:
 *   - "Drive low"  → configure pin as OUTPUT, write 0
 *   - "Release"    → configure pin as INPUT (external pull-up holds line HIGH)
 *
 * CRC: Dallas/Maxim CRC-8, polynomial x^8 + x^5 + x^4 + 1 (0x31),
 *      initial value 0x00, input and output NOT reflected.
 */

#include <l4/ds18b20/ds18b20.h>
#include <l4/sys/kip.h>
#include <l4/re/env.h>
#include <l4/util/util.h>

#include <cstdio>

/* ------------------------------------------------------------------ */
/* 1-Wire ROM / function command bytes                                  */
/* ------------------------------------------------------------------ */
static constexpr l4_uint8_t CMD_READ_ROM       = 0x33;
static constexpr l4_uint8_t CMD_MATCH_ROM      = 0x55;
static constexpr l4_uint8_t CMD_SKIP_ROM       = 0xCC;

static constexpr l4_uint8_t CMD_CONVERT_T      = 0x44;
static constexpr l4_uint8_t CMD_READ_SCRATCH   = 0xBE;
static constexpr l4_uint8_t CMD_WRITE_SCRATCH  = 0x4E;
static constexpr l4_uint8_t CMD_COPY_SCRATCH   = 0x48;

/* Configuration register bits for resolution */
static constexpr l4_uint8_t CFG_9BIT  = 0x1F;
static constexpr l4_uint8_t CFG_10BIT = 0x3F;
static constexpr l4_uint8_t CFG_11BIT = 0x5F;
static constexpr l4_uint8_t CFG_12BIT = 0x7F;

/* Conversion time in ms per resolution setting */
static constexpr unsigned CONV_MS_9BIT  = 94;
static constexpr unsigned CONV_MS_10BIT = 188;
static constexpr unsigned CONV_MS_11BIT = 375;
static constexpr unsigned CONV_MS_12BIT = 750;

/* ------------------------------------------------------------------ */
/* Construction                                                          */
/* ------------------------------------------------------------------ */

Ds18b20::Ds18b20(L4vbus::Gpio_pin pin)
: _pin(pin)
{
  /* Start with line released (input / high) */
  pin_release();
}

/* ------------------------------------------------------------------ */
/* GPIO open-drain helpers                                              */
/* ------------------------------------------------------------------ */

void Ds18b20::pin_low()
{
  _pin.setup(L4VBUS_GPIO_SETUP_OUTPUT, 0);
}

void Ds18b20::pin_release()
{
  _pin.setup(L4VBUS_GPIO_SETUP_INPUT, 0);
}

int Ds18b20::pin_read()
{
  return _pin.get();
}

/* ------------------------------------------------------------------ */
/* Microsecond busy-wait using the KIP clock                            */
/* ------------------------------------------------------------------ */

void Ds18b20::delay_us(unsigned us)
{
  l4_uint64_t end = l4_kip_clock_ns(l4re_kip())
                    + static_cast<l4_uint64_t>(us) * 1000ULL;
  while (l4_kip_clock_ns(l4re_kip()) < end)
    ; /* busy-wait */
}

/* ------------------------------------------------------------------ */
/* Dallas/Maxim CRC-8  (poly 0x31, init 0x00)                          */
/* ------------------------------------------------------------------ */

l4_uint8_t Ds18b20::crc8(l4_uint8_t const *data, unsigned len)
{
  l4_uint8_t crc = 0x00;
  for (unsigned i = 0; i < len; ++i)
    {
      l4_uint8_t byte = data[i];
      for (unsigned b = 0; b < 8; ++b)
        {
          l4_uint8_t mix = (crc ^ byte) & 0x01;
          crc >>= 1;
          if (mix)
            crc ^= 0x8C;   /* reflected 0x31 */
          byte >>= 1;
        }
    }
  return crc;
}

/* ------------------------------------------------------------------ */
/* 1-Wire bus primitives                                                */
/* ------------------------------------------------------------------ */

bool Ds18b20::reset()
{
  /* Drive low for 480 µs */
  pin_low();
  delay_us(480);

  /* Release and wait 70 µs before sampling */
  pin_release();
  delay_us(70);

  /* Sample: device pulls low during its 60–240 µs presence pulse */
  int presence = (pin_read() == 0) ? 1 : 0;

  /* Wait out the rest of the reset / presence window (≥ 480 µs total) */
  delay_us(410);

  return presence != 0;
}

void Ds18b20::write_bit(bool b)
{
  if (b)
    {
      /* Write-1: drive low 6 µs, release for 54 µs */
      pin_low();
      delay_us(6);
      pin_release();
      delay_us(54);
    }
  else
    {
      /* Write-0: drive low 60 µs, release for 10 µs recovery */
      pin_low();
      delay_us(60);
      pin_release();
      delay_us(10);
    }
}

bool Ds18b20::read_bit()
{
  /* Initiate read slot: drive low ≥ 1 µs */
  pin_low();
  delay_us(3);

  /* Release and sample within 15 µs of slot start */
  pin_release();
  delay_us(7);   /* 3 + 7 = 10 µs from slot start */

  int val = pin_read();

  /* Complete the 60 µs slot */
  delay_us(50);

  return val != 0;
}

void Ds18b20::write_byte(l4_uint8_t byte)
{
  for (unsigned i = 0; i < 8; ++i)
    {
      write_bit((byte >> i) & 0x01);
    }
}

l4_uint8_t Ds18b20::read_byte()
{
  l4_uint8_t byte = 0;
  for (unsigned i = 0; i < 8; ++i)
    {
      if (read_bit())
        byte |= (1u << i);
    }
  return byte;
}

/* ------------------------------------------------------------------ */
/* ROM addressing                                                        */
/* ------------------------------------------------------------------ */

bool Ds18b20::select()
{
  if (!reset())
    return false;

  if (_rom == Skip_rom)
    {
      write_byte(CMD_SKIP_ROM);
    }
  else
    {
      write_byte(CMD_MATCH_ROM);
      /* ROM code is 8 bytes, LSB first */
      for (unsigned i = 0; i < 8; ++i)
        write_byte(static_cast<l4_uint8_t>(_rom >> (i * 8)));
    }

  return true;
}

/* ------------------------------------------------------------------ */
/* Scratchpad                                                            */
/* ------------------------------------------------------------------ */

long Ds18b20::read_scratchpad(l4_uint8_t buf[9])
{
  for (unsigned i = 0; i < 9; ++i)
    buf[i] = read_byte();

  if (crc8(buf, 8) != buf[8])
    {
      printf("ds18b20: CRC mismatch (got 0x%02x, expected 0x%02x)\n",
             crc8(buf, 8), buf[8]);
      return -L4_EIO;
    }

  return L4_EOK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                            */
/* ------------------------------------------------------------------ */

bool Ds18b20::present()
{
  return reset();
}

long Ds18b20::read_rom(Rom_code *rom)
{
  if (!reset())
    return -L4_EIO;

  write_byte(CMD_READ_ROM);

  l4_uint8_t buf[8];
  for (unsigned i = 0; i < 8; ++i)
    buf[i] = read_byte();

  if (crc8(buf, 7) != buf[7])
    {
      printf("ds18b20: ROM CRC mismatch\n");
      return -L4_EIO;
    }

  Rom_code code = 0;
  for (unsigned i = 0; i < 8; ++i)
    code |= static_cast<Rom_code>(buf[i]) << (i * 8);

  *rom = code;
  return L4_EOK;
}

long Ds18b20::set_resolution(Resolution res)
{
  /* Read current scratchpad to preserve TH/TL alarm bytes */
  if (!select())
    return -L4_EIO;
  write_byte(CMD_READ_SCRATCH);

  l4_uint8_t sp[9];
  if (read_scratchpad(sp) != L4_EOK)
    return -L4_EIO;

  l4_uint8_t cfg;
  switch (res)
    {
    case Res_9bit:  cfg = CFG_9BIT;  break;
    case Res_10bit: cfg = CFG_10BIT; break;
    case Res_11bit: cfg = CFG_11BIT; break;
    default:        cfg = CFG_12BIT; break;
    }

  /* Write scratchpad: TH, TL, config */
  if (!select())
    return -L4_EIO;
  write_byte(CMD_WRITE_SCRATCH);
  write_byte(sp[2]);   /* TH: preserve */
  write_byte(sp[3]);   /* TL: preserve */
  write_byte(cfg);

  /* Copy scratchpad to EEPROM */
  if (!select())
    return -L4_EIO;
  write_byte(CMD_COPY_SCRATCH);
  l4_usleep(10000);    /* EEPROM write time: max 10 ms */

  return L4_EOK;
}

long Ds18b20::read_temp_raw(l4_int16_t *raw)
{
  /* Trigger conversion */
  if (!select())
    return -L4_EIO;
  write_byte(CMD_CONVERT_T);

  /*
   * Wait for conversion. We use the maximum 12-bit time; for lower
   * resolutions the device signals completion by returning 1 on read-bit,
   * but polling the line requires parasitic power which we avoid.
   */
  l4_usleep(CONV_MS_12BIT * 1000ULL);

  /* Read scratchpad */
  if (!select())
    return -L4_EIO;
  write_byte(CMD_READ_SCRATCH);

  l4_uint8_t sp[9];
  if (read_scratchpad(sp) != L4_EOK)
    return -L4_EIO;

  *raw = static_cast<l4_int16_t>(
           static_cast<l4_uint16_t>(sp[0]) |
           (static_cast<l4_uint16_t>(sp[1]) << 8));
  return L4_EOK;
}

long Ds18b20::read_temp_c100(int *temp_c100)
{
  l4_int16_t raw;
  long r = read_temp_raw(&raw);
  if (r < 0)
    return r;

  /*
   * Raw value is a signed fixed-point number with 4 fractional bits:
   *   temperature [°C] = raw / 16.0
   *   temperature [1/100 °C] = raw * 100 / 16 = raw * 25 / 4
   *
   * Use integer arithmetic; scale to 1/100 °C (round toward zero).
   */
  *temp_c100 = (static_cast<int>(raw) * 25) / 4;
  return L4_EOK;
}
