/*
 * SSD1306 OLED display demo.
 *
 * All parameters (CS, mode, speed, D/C pin) are set via Kconfig.
 * See pkg/devices/ssd1306/Kconfig.L4 for the full list.
 */

#include <l4/ssd1306/ssd1306.h>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_gpio>

#include <cstdio>
#include <cstring>

// ---- Defaults from Kconfig (fallbacks for standalone build) ----

#ifndef CONFIG_SSD1306_SPI_CS
#  define CONFIG_SSD1306_SPI_CS    0
#endif

#ifndef CONFIG_SSD1306_SPI_MODE
#  define CONFIG_SSD1306_SPI_MODE  0
#endif

#ifndef CONFIG_SSD1306_SPI_SPEED_KHZ
#  define CONFIG_SSD1306_SPI_SPEED_KHZ  8000
#endif

#ifndef CONFIG_SSD1306_DC_PIN
#  define CONFIG_SSD1306_DC_PIN    25
#endif

#ifndef CONFIG_SSD1306_HEIGHT
#  define CONFIG_SSD1306_HEIGHT    64
#endif

// Build factory->create() argument strings from Kconfig values
#define _STR(x)  #x
#define _XSTR(x) _STR(x)

static L4::Cap<Spi_device_ops>
create_spi_device(L4::Cap<L4::Factory> factory)
{
  auto dev = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<Spi_device_ops>(),
                          "Alloc SPI device cap");

  char cs_str[16], mode_str[12], speed_str[24];
  snprintf(cs_str,    sizeof(cs_str),    "cs=%d",    CONFIG_SSD1306_SPI_CS);
  snprintf(mode_str,  sizeof(mode_str),  "mode=%d",  CONFIG_SSD1306_SPI_MODE);
  snprintf(speed_str, sizeof(speed_str), "speed=%d", CONFIG_SSD1306_SPI_SPEED_KHZ * 1000);

  L4Re::chksys(
    l4_error(factory->create(dev, 1L /* Type_rpc */)
             << static_cast<char const *>(cs_str)
             << static_cast<char const *>(mode_str)
             << static_cast<char const *>(speed_str)),
    "Create SPI device");
  return dev;
}

int main()
{
  printf("ssd1306-example: starting\n");
  printf("  display : %ux%u\n", Ssd1306::Width, Ssd1306::Height);
  printf("  SPI CS  : %d, mode %d, speed %d kHz\n",
         CONFIG_SSD1306_SPI_CS, CONFIG_SSD1306_SPI_MODE,
         CONFIG_SSD1306_SPI_SPEED_KHZ);
  printf("  D/C pin : %d\n", CONFIG_SSD1306_DC_PIN);

  // Obtain SPI factory capability
  auto spi_factory = L4Re::Env::env()->get_cap<L4::Factory>("spi");
  if (!spi_factory)
    {
      printf("Error: 'spi' capability not found in environment\n");
      return 1;
    }

  // Create SPI device via factory
  auto spi_dev = create_spi_device(spi_factory);

  // Obtain vbus and find GPIO device for D/C pin
  auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (!vbus)
    {
      printf("Error: 'vbus' capability not found\n");
      return 1;
    }

  L4vbus::Device gpio_dev;
  L4Re::chksys(vbus->root().device_by_hid(&gpio_dev, "gpio"),
               "Find GPIO device on vbus");

  L4vbus::Gpio_pin dc(gpio_dev, CONFIG_SSD1306_DC_PIN);
  dc.setup(L4VBUS_GPIO_SETUP_OUTPUT, 0);

  // Create driver and initialize display
  Ssd1306 oled(spi_dev, dc);
  long r = oled.init();
  if (r < 0)
    {
      printf("Error: SSD1306 init failed: %ld\n", r);
      return 1;
    }

  // Demo: clear and draw text
  oled.clear();

  oled.set_cursor(0, 0);
  oled.draw_string("Hello TuringOS!");

  oled.set_cursor(0, 16);
  oled.draw_string("SSD1306 "
                   _XSTR(128) "x" _XSTR(CONFIG_SSD1306_HEIGHT));

  // Draw a horizontal line across the screen
  unsigned mid_y = Ssd1306::Height / 2 - 2;
  for (unsigned x = 0; x < Ssd1306::Width; ++x)
    oled.set_pixel(x, mid_y, true);

  // Draw a rectangle in the lower half
  unsigned ry0 = Ssd1306::Height / 2;
  unsigned ry1 = Ssd1306::Height - 2;
  for (unsigned x = 10; x <= 117; ++x)
    {
      oled.set_pixel(x, ry0, true);
      oled.set_pixel(x, ry1, true);
    }
  for (unsigned y = ry0; y <= ry1; ++y)
    {
      oled.set_pixel(10, y, true);
      oled.set_pixel(117, y, true);
    }

  // Diagonal line inside the rectangle
  unsigned rect_h = ry1 - ry0;
  for (unsigned i = 0; i <= rect_h; ++i)
    oled.set_pixel(11 + i * 106 / rect_h, ry0 + 1 + i, true);

  oled.display();

  printf("ssd1306-example: display updated\n");
  return 0;
}
