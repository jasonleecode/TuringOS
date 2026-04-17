/*
 * MCP2515 CAN controller demo.
 *
 * Tests the driver using the MCP2515's built-in loopback mode so that
 * no physical CAN bus is required.  The program:
 *
 *   1. Initialises the MCP2515 (500 kbps, 8 MHz crystal by default).
 *   2. Switches to loopback mode (TX feeds directly back into RX).
 *   3. Sends a standard frame (ID 0x123, 4 bytes of payload).
 *   4. Sends an extended frame (ID 0x1ABCDEF, 8 bytes of payload).
 *   5. Receives and prints both frames.
 *   6. Reports the TX/RX error counters.
 *
 * All parameters are taken from Kconfig with compile-time fallbacks so
 * the example can also be built outside the full Kconfig flow.
 *
 * The 'spi' capability in the environment must be created by the
 * spi-driver server before this program runs.
 */

#include <l4/mcp2515/mcp2515.h>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory>

#include <cstdio>

// ---- Kconfig fallbacks ----

#ifndef CONFIG_MCP2515_SPI_CS
#  define CONFIG_MCP2515_SPI_CS       0
#endif

#ifndef CONFIG_MCP2515_SPI_SPEED_KHZ
#  define CONFIG_MCP2515_SPI_SPEED_KHZ 4000
#endif

#ifndef CONFIG_MCP2515_OSC_16MHZ
#  define MCP2515_OSC Mcp2515::Osc_freq::MHz8
#else
#  define MCP2515_OSC Mcp2515::Osc_freq::MHz16
#endif

#if defined(CONFIG_MCP2515_BITRATE_125K)
#  define MCP2515_RATE Mcp2515::Bit_rate::kbps_125
#elif defined(CONFIG_MCP2515_BITRATE_250K)
#  define MCP2515_RATE Mcp2515::Bit_rate::kbps_250
#elif defined(CONFIG_MCP2515_BITRATE_1000K)
#  define MCP2515_RATE Mcp2515::Bit_rate::kbps_1000
#else
#  define MCP2515_RATE Mcp2515::Bit_rate::kbps_500
#endif

#define _STR(x)  #x
#define _XSTR(x) _STR(x)

static L4::Cap<Spi_device_ops>
create_spi_device(L4::Cap<L4::Factory> factory)
{
  auto dev = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<Spi_device_ops>(),
                          "Alloc SPI device cap");

  char cs_str[16], speed_str[24];
  snprintf(cs_str,    sizeof(cs_str),    "cs=%d",    CONFIG_MCP2515_SPI_CS);
  snprintf(speed_str, sizeof(speed_str), "speed=%d", CONFIG_MCP2515_SPI_SPEED_KHZ * 1000);

  /* factory->create() returns a stream; push key=value strings with << */
  L4Re::chksys(
    l4_error(factory->create(dev, 1L /* Type_rpc */)
             << static_cast<char const *>(cs_str)
             << "mode=0"
             << static_cast<char const *>(speed_str)),
    "Create SPI device for MCP2515");

  return dev;
}

static void print_frame(char const *tag, Mcp2515::Can_frame const &f)
{
  printf("  %s: id=0x%lX %s%s dlc=%u data=",
         tag,
         static_cast<unsigned long>(f.id),
         f.extended ? "[ext]" : "[std]",
         f.rtr      ? "[rtr]" : "",
         f.dlc);
  for (unsigned i = 0; i < f.dlc; ++i)
    printf("%02X ", f.data[i]);
  printf("\n");
}

int main()
{
  printf("mcp2515-example: starting\n");
  printf("  SPI CS    : %d, mode 0, speed %d kHz\n",
         CONFIG_MCP2515_SPI_CS, CONFIG_MCP2515_SPI_SPEED_KHZ);

  auto spi_factory = L4Re::Env::env()->get_cap<L4::Factory>("spi");
  if (!spi_factory)
    {
      printf("Error: 'spi' capability not found\n");
      return 1;
    }

  auto spi_dev = create_spi_device(spi_factory);

  Mcp2515 can(spi_dev, MCP2515_OSC);

  // Initialise and immediately switch to loopback for self-test
  long r = can.init(MCP2515_RATE);
  if (r < 0)
    {
      printf("Error: MCP2515 init failed: %ld\n", r);
      return 1;
    }

  r = can.set_mode_loopback();
  if (r < 0)
    {
      printf("Error: loopback mode switch failed: %ld\n", r);
      return 1;
    }
  printf("  mode      : loopback\n\n");

  // ---- Send standard frame ----

  Mcp2515::Can_frame tx0 = {};
  tx0.id       = 0x123;
  tx0.extended = false;
  tx0.rtr      = false;
  tx0.dlc      = 4;
  tx0.data[0]  = 0xDE;
  tx0.data[1]  = 0xAD;
  tx0.data[2]  = 0xBE;
  tx0.data[3]  = 0xEF;

  r = can.send(tx0);
  if (r < 0)
    {
      printf("Error: send (std frame) failed: %ld\n", r);
      return 1;
    }
  print_frame("TX std", tx0);

  // ---- Receive standard frame ----

  if (can.rx_pending())
    {
      Mcp2515::Can_frame rx0 = {};
      r = can.recv(rx0);
      if (r == L4_EOK)
        print_frame("RX std", rx0);
      else
        printf("  RX std: recv error %ld\n", r);
    }
  else
    {
      printf("  RX std: no message pending (loopback may need a moment)\n");
    }

  // ---- Send extended frame ----

  Mcp2515::Can_frame tx1 = {};
  tx1.id       = 0x1ABCDEF;
  tx1.extended = true;
  tx1.rtr      = false;
  tx1.dlc      = 8;
  for (unsigned i = 0; i < 8; ++i)
    tx1.data[i] = static_cast<l4_uint8_t>(0x10 + i);

  r = can.send(tx1);
  if (r < 0)
    {
      printf("Error: send (ext frame) failed: %ld\n", r);
      return 1;
    }
  print_frame("TX ext", tx1);

  // ---- Receive extended frame ----

  if (can.rx_pending())
    {
      Mcp2515::Can_frame rx1 = {};
      r = can.recv(rx1);
      if (r == L4_EOK)
        print_frame("RX ext", rx1);
      else
        printf("  RX ext: recv error %ld\n", r);
    }
  else
    {
      printf("  RX ext: no message pending\n");
    }

  // ---- Error counters ----

  l4_uint8_t tec = 0, rec = 0;
  if (can.get_error_counters(&tec, &rec) == L4_EOK)
    printf("\n  TEC=%u  REC=%u\n", tec, rec);

  printf("mcp2515-example: done\n");
  return 0;
}
