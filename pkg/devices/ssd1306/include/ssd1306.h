/*
 * SSD1306 OLED display driver (SPI).
 *
 * Supports 128x64 and 128x32 variants. Display height is selected
 * via CONFIG_SSD1306_HEIGHT (set in Kconfig), defaulting to 64.
 */
#pragma once

#include <l4/sys/types.h>
#include <l4/sys/capability>
#include <l4/spi-driver/spi_device_if.h>
#include <l4/vbus/vbus_gpio>

#ifdef CONFIG_SSD1306_HEIGHT
#  define SSD1306_DISPLAY_HEIGHT CONFIG_SSD1306_HEIGHT
#else
#  define SSD1306_DISPLAY_HEIGHT 64
#endif

#ifdef CONFIG_SSD1306_CONTRAST
#  define SSD1306_DEFAULT_CONTRAST CONFIG_SSD1306_CONTRAST
#else
#  define SSD1306_DEFAULT_CONTRAST 0xCF
#endif

class Ssd1306
{
public:
  static constexpr unsigned Width   = 128;
  static constexpr unsigned Height  = SSD1306_DISPLAY_HEIGHT;
  static constexpr unsigned Pages   = Height / 8;
  static constexpr unsigned Fb_size = Width * Pages;

  Ssd1306(L4::Cap<Spi_device_ops> spi, L4vbus::Gpio_pin dc_pin);

  long init();
  void clear();
  void display();

  void set_pixel(unsigned x, unsigned y, bool on);
  void fill(l4_uint8_t pattern);
  void set_contrast(l4_uint8_t val);
  void invert_display(bool inv);
  void display_on(bool on);

  void set_cursor(unsigned x, unsigned y);
  void draw_char(char c);
  void draw_string(char const *s);

  l4_uint8_t *framebuffer() { return _fb; }

private:
  long send_cmd(l4_uint8_t cmd);
  long send_cmd_seq(l4_uint8_t const *cmds, unsigned len);
  long send_data(l4_uint8_t const *data, unsigned len);

  L4::Cap<Spi_device_ops> _spi;
  L4vbus::Gpio_pin        _dc;
  l4_uint8_t              _fb[Fb_size];
  unsigned                _cursor_x = 0;
  unsigned                _cursor_y = 0;
};
