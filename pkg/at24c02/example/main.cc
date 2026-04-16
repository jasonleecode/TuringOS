/*
 * AT24C02 EEPROM demonstration.
 *
 * Expects two capabilities in the environment:
 *   i2c  – factory capability from the i2c-driver server (Type_rpc = 1)
 *
 * The example:
 *   1. Creates an I2C device endpoint for the AT24C02 at its default
 *      address (0x50).
 *   2. Writes a 16-byte test pattern to addresses 0x00–0x0F.
 *   3. Reads the data back and verifies each byte.
 *   4. Writes a short ASCII string at address 0x10 and reads it back.
 *   5. Demonstrates single-byte read/write at a specific address.
 */

#include <l4/at24c02/at24c02.h>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory>

#include <cstdio>
#include <cstring>

/* Obtain an I2C device cap from the i2c-driver factory for the AT24C02 */
static L4::Cap<I2c_device_ops>
create_i2c_device(L4::Cap<L4::Factory> factory, l4_uint8_t addr)
{
  auto dev = L4Re::chkcap(
    L4Re::Util::cap_alloc.alloc<I2c_device_ops>(),
    "Alloc I2C device cap");

  /* addr= argument is parsed as a hex number by i2c-driver */
  char addr_str[16];
  snprintf(addr_str, sizeof(addr_str), "addr=%x",
           static_cast<unsigned>(addr));

  /* factory->create() returns a stream; use << to append string arguments */
  L4Re::chksys(
    l4_error(factory->create(dev, 1L /* Type_rpc */)
             << static_cast<char const *>(addr_str)),
    "Create I2C device for AT24C02");

  return dev;
}

int main()
{
  printf("AT24C02 EEPROM example\n");
  printf("  I2C address : 0x%02x\n", At24c02::Base_addr);
  printf("  Capacity    : %u bytes (%u pages x %u bytes)\n",
         At24c02::Capacity, At24c02::Num_pages, At24c02::Page_size);

  /* ---- Obtain i2c factory ---- */
  auto i2c_factory = L4Re::Env::env()->get_cap<L4::Factory>("i2c");
  if (!i2c_factory)
    {
      printf("Error: 'i2c' capability not found in environment\n");
      return 1;
    }

  /* ---- Create device endpoint for AT24C02 at default address ---- */
  L4::Cap<I2c_device_ops> i2c_dev;
  try
    {
      i2c_dev = create_i2c_device(i2c_factory, At24c02::Base_addr);
    }
  catch (L4::Runtime_error const &e)
    {
      printf("Error: could not create I2C device: %s\n", e.str());
      return 1;
    }

  At24c02 eeprom(i2c_dev);

  /* ---- Test 1: write and read back a 16-byte pattern ---- */
  printf("\n[Test 1] Write/read 16-byte pattern at 0x00\n");
  {
    l4_uint8_t wbuf[16];
    for (unsigned i = 0; i < sizeof(wbuf); ++i)
      wbuf[i] = static_cast<l4_uint8_t>(0xA0 + i);

    long r = eeprom.write(0x00, wbuf, sizeof(wbuf));
    if (r < 0)
      {
        printf("  Write failed: %ld\n", r);
        return 1;
      }

    l4_uint8_t rbuf[16] = {};
    r = eeprom.read(0x00, rbuf, sizeof(rbuf));
    if (r < 0)
      {
        printf("  Read failed: %ld\n", r);
        return 1;
      }

    bool ok = (memcmp(wbuf, rbuf, sizeof(wbuf)) == 0);
    printf("  %s", ok ? "PASS" : "FAIL");
    if (!ok)
      {
        printf(" — mismatch:");
        for (unsigned i = 0; i < sizeof(rbuf); ++i)
          if (rbuf[i] != wbuf[i])
            printf(" [%u]: wrote 0x%02x, got 0x%02x", i, wbuf[i], rbuf[i]);
      }
    printf("\n");
    if (!ok)
      return 1;
  }

  /* ---- Test 2: write and read back an ASCII string ---- */
  printf("\n[Test 2] Write/read ASCII string at 0x10\n");
  {
    char const msg[] = "TuringOS";
    unsigned  msglen = sizeof(msg); /* includes NUL */

    long r = eeprom.write(0x10,
                          reinterpret_cast<l4_uint8_t const *>(msg),
                          msglen);
    if (r < 0)
      {
        printf("  Write failed: %ld\n", r);
        return 1;
      }

    char rbuf[sizeof(msg)] = {};
    r = eeprom.read(0x10, reinterpret_cast<l4_uint8_t *>(rbuf), msglen);
    if (r < 0)
      {
        printf("  Read failed: %ld\n", r);
        return 1;
      }

    bool ok = (strcmp(msg, rbuf) == 0);
    printf("  %s — read back: \"%s\"\n", ok ? "PASS" : "FAIL", rbuf);
    if (!ok)
      return 1;
  }

  /* ---- Test 3: single-byte read/write ---- */
  printf("\n[Test 3] Single-byte write/read at 0x20\n");
  {
    l4_uint8_t val_w = 0xBE;
    long r = eeprom.write_byte(0x20, val_w);
    if (r < 0)
      {
        printf("  write_byte failed: %ld\n", r);
        return 1;
      }

    l4_uint8_t val_r = 0x00;
    r = eeprom.read_byte(0x20, &val_r);
    if (r < 0)
      {
        printf("  read_byte failed: %ld\n", r);
        return 1;
      }

    bool ok = (val_r == val_w);
    printf("  %s — wrote 0x%02x, read 0x%02x\n",
           ok ? "PASS" : "FAIL", val_w, val_r);
    if (!ok)
      return 1;
  }

  printf("\nAll tests passed.\n");
  return 0;
}
