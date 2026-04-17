/*
 * TEF6686HN radio tuner demo.
 *
 * Demonstrates initialisation, FM/AM tuning, quality readout, RDS
 * reception, and a simple FM seek.
 *
 * The 'i2c' capability must be provided by the i2c-driver server and
 * the factory must resolve address 0x1C (the TEF6686HN I2C address).
 */

#include <l4/tef6686hn/tef6686hn.h>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory>
#include <l4/util/util.h>

#include <cstdio>

#ifndef CONFIG_TEF6686HN_I2C_ADDR
#  define CONFIG_TEF6686HN_I2C_ADDR 0x1C
#endif

static L4::Cap<I2c_device_ops>
create_i2c_device(L4::Cap<L4::Factory> factory, l4_uint8_t addr)
{
  auto dev = L4Re::chkcap(
    L4Re::Util::cap_alloc.alloc<I2c_device_ops>(),
    "Alloc I2C device cap");

  char addr_str[16];
  snprintf(addr_str, sizeof(addr_str), "addr=%x",
           static_cast<unsigned>(addr));

  L4Re::chksys(
    l4_error(factory->create(dev, 1L /* Type_rpc */)
             << static_cast<char const *>(addr_str)),
    "Create I2C device for TEF6686HN");

  return dev;
}

static void print_quality(Tef6686hn &radio, char const *label)
{
  Tef6686hn::Quality q = {};
  long r = radio.get_quality(&q);
  if (r < 0)
    {
      printf("  %s: quality read error %ld\n", label, r);
      return;
    }
  printf("  %-12s  RSSI %5.1f dBuV   SNR %5.1f dB"
         "   MP %3d   offset %+d Hz\n",
         label,
         q.rssi        / 10.0,
         q.snr         / 10.0,
         static_cast<int>(q.multipath),
         static_cast<int>(q.freq_offset) * 10);
}

static void demo_fm(Tef6686hn &radio)
{
  printf("\n=== FM demo ===\n");

  struct { l4_uint32_t freq; char const *name; } stations[] = {
    { 98000,  "98.0 MHz" },
    { 100300, "100.3 MHz" },
    { 104700, "104.7 MHz" },
  };

  for (auto &s : stations)
    {
      long r = radio.tune(Tef6686hn::Band::FM, s.freq);
      if (r < 0)
        {
          printf("  tune(%s) failed: %ld\n", s.name, r);
          continue;
        }
      l4_usleep(50000); /* 50 ms settle time */
      print_quality(radio, s.name);

      l4_uint32_t actual = 0;
      if (radio.get_frequency(&actual) == L4_EOK)
        printf("  %-12s  AFC settled at %u.%u MHz\n",
               "", actual / 1000, (actual % 1000) / 100);
    }
}

static void demo_am(Tef6686hn &radio)
{
  printf("\n=== AM (MW) demo ===\n");

  struct { l4_uint32_t freq; char const *name; } stations[] = {
    {  999, "999 kHz"  },
    { 1080, "1080 kHz" },
    { 1512, "1512 kHz" },
  };

  for (auto &s : stations)
    {
      long r = radio.tune(Tef6686hn::Band::MW, s.freq);
      if (r < 0)
        {
          printf("  tune(%s) failed: %ld\n", s.name, r);
          continue;
        }
      l4_usleep(50000);
      print_quality(radio, s.name);
    }
}

static void demo_seek(Tef6686hn &radio)
{
  printf("\n=== FM seek demo (seek up from 88.0 MHz) ===\n");

  long r = radio.tune(Tef6686hn::Band::FM, 88000);
  if (r < 0)
    {
      printf("  initial tune failed: %ld\n", r);
      return;
    }

  r = radio.seek(Tef6686hn::Seek_dir::Up, 200 /* 20.0 dBuV */);
  if (r == L4_EOK)
    {
      l4_uint32_t freq = 0;
      radio.get_frequency(&freq);
      printf("  found station at %u.%u MHz\n", freq / 1000, (freq % 1000) / 100);
      print_quality(radio, "seeked");
    }
  else if (r == -L4_ENOENT)
    {
      printf("  no station found\n");
    }
  else
    {
      printf("  seek error: %ld\n", r);
    }
}

static void demo_rds(Tef6686hn &radio, l4_uint32_t freq_khz)
{
  printf("\n=== RDS demo at %u.%u MHz ===\n",
         freq_khz / 1000, (freq_khz % 1000) / 100);

  long r = radio.tune(Tef6686hn::Band::FM, freq_khz);
  if (r < 0)
    {
      printf("  tune failed: %ld\n", r);
      return;
    }

  /* Wait 1 s for RDS groups to accumulate */
  l4_usleep(1000000);

  unsigned groups_found = 0;
  for (unsigned i = 0; i < 10; ++i)
    {
      Tef6686hn::Rds_group g = {};
      r = radio.get_rds_group(&g);
      if (r < 0)
        {
          printf("  RDS read error: %ld\n", r);
          break;
        }
      if (!g.valid)
        {
          l4_usleep(50000);
          continue;
        }

      ++groups_found;
      printf("  [RDS] A=%04X B=%04X C=%04X D=%04X err=%02X\n",
             g.block[0], g.block[1], g.block[2], g.block[3], g.err_bits);

      /* Decode group type from block B bits 15:12 */
      unsigned group_type  = (g.block[1] >> 12) & 0xF;
      unsigned version_bit = (g.block[1] >> 11) & 0x1;
      printf("         group %uB\n", group_type);
      if (group_type == 0 && !version_bit)
        {
          /* Group 0A: programme service name (2 chars per group) */
          unsigned seg  = g.block[1] & 0x03;
          char c0 = static_cast<char>(g.block[3] >> 8);
          char c1 = static_cast<char>(g.block[3] & 0xFF);
          printf("         PS[%u%u] = '%c%c'\n",
                 seg * 2, seg * 2 + 1,
                 (c0 >= 0x20 && c0 < 0x7F) ? c0 : '?',
                 (c1 >= 0x20 && c1 < 0x7F) ? c1 : '?');
        }
    }

  if (groups_found == 0)
    printf("  no RDS data received (station may not broadcast RDS)\n");
}

int main()
{
  printf("tef6686hn-example: starting\n");
  printf("  I2C address : 0x%02X\n\n", CONFIG_TEF6686HN_I2C_ADDR);

  auto i2c_factory = L4Re::Env::env()->get_cap<L4::Factory>("i2c");
  if (!i2c_factory)
    {
      printf("Error: 'i2c' capability not found in environment\n");
      return 1;
    }

  L4::Cap<I2c_device_ops> i2c_dev;
  try
    {
      i2c_dev = create_i2c_device(i2c_factory, CONFIG_TEF6686HN_I2C_ADDR);
    }
  catch (L4::Runtime_error const &e)
    {
      printf("Error: I2C device create failed: %s\n", e.str());
      return 1;
    }

  Tef6686hn radio(i2c_dev);

  long r = radio.init();
  if (r < 0)
    {
      printf("Error: TEF6686HN init failed: %ld\n", r);
      return 1;
    }
  printf("  chip initialised\n");

  demo_fm(radio);
  demo_am(radio);
  demo_seek(radio);
  demo_rds(radio, 98000);

  radio.mute(true);
  printf("\ntef6686hn-example: done\n");
  return 0;
}
