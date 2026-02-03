/*
 * SSD1306 OLED display driver implementation (SPI).
 *
 * Supports 128x64 and 128x32 via Kconfig (CONFIG_SSD1306_HEIGHT).
 */

#include <l4/ssd1306/ssd1306.h>
#include <cstring>

#include "font5x7.h"

/*
 * Build the initialization sequence at compile time based on display height.
 *
 *   128x64: multiplex=0x3F, COM pins=0x12 (alternative, no remap)
 *   128x32: multiplex=0x1F, COM pins=0x02 (sequential,  no remap)
 */
static constexpr l4_uint8_t mux_ratio =
  (Ssd1306::Height == 64) ? 0x3F : 0x1F;

static constexpr l4_uint8_t com_pins =
  (Ssd1306::Height == 64) ? 0x12 : 0x02;

static l4_uint8_t const init_cmds[] = {
  0xAE,                          // Display OFF
  0xD5, 0x80,                    // Set clock div ratio
  0xA8, mux_ratio,               // Set multiplex
  0xD3, 0x00,                    // Set display offset = 0
  0x40,                          // Set start line = 0
  0x8D, 0x14,                    // Charge pump ON (internal VCC)
  0x20, 0x00,                    // Horizontal addressing mode
  0xA1,                          // Segment remap (col 127 = SEG0)
  0xC8,                          // COM scan descending
  0xDA, com_pins,                // COM pins configuration
  0x81, SSD1306_DEFAULT_CONTRAST,// Contrast
  0xD9, 0xF1,                    // Pre-charge period
  0xDB, 0x40,                    // VCOMH deselect level
  0xA4,                          // Display from RAM
  0xA6,                          // Normal display (not inverted)
  0xAF,                          // Display ON
};

Ssd1306::Ssd1306(L4::Cap<Spi_device_ops> spi, L4vbus::Gpio_pin dc_pin)
: _spi(spi), _dc(dc_pin), _cursor_x(0), _cursor_y(0)
{
  memset(_fb, 0, Fb_size);
}

long Ssd1306::send_cmd(l4_uint8_t cmd)
{
  _dc.set(0);
  L4::Ipc::Array<l4_uint8_t const> buf(&cmd, 1);
  return _spi->write(buf);
}

long Ssd1306::send_cmd_seq(l4_uint8_t const *cmds, unsigned len)
{
  for (unsigned i = 0; i < len; ++i)
    {
      long r = send_cmd(cmds[i]);
      if (r < 0)
        return r;
    }
  return L4_EOK;
}

long Ssd1306::send_data(l4_uint8_t const *data, unsigned len)
{
  _dc.set(1);
  L4::Ipc::Array<l4_uint8_t const> buf(data, len);
  return _spi->write(buf);
}

long Ssd1306::init()
{
  return send_cmd_seq(init_cmds, sizeof(init_cmds));
}

void Ssd1306::clear()
{
  memset(_fb, 0, Fb_size);
}

void Ssd1306::display()
{
  // Set column address range 0-127
  l4_uint8_t col_cmds[] = { 0x21, 0x00, 0x7F };
  send_cmd_seq(col_cmds, sizeof(col_cmds));

  // Set page address range 0 to (Pages-1)
  l4_uint8_t page_cmds[] = { 0x22, 0x00,
                             static_cast<l4_uint8_t>(Pages - 1) };
  send_cmd_seq(page_cmds, sizeof(page_cmds));

  send_data(_fb, Fb_size);
}

void Ssd1306::set_pixel(unsigned x, unsigned y, bool on)
{
  if (x >= Width || y >= Height)
    return;

  unsigned page = y / 8;
  unsigned bit  = y % 8;

  if (on)
    _fb[page * Width + x] |= (1u << bit);
  else
    _fb[page * Width + x] &= ~(1u << bit);
}

void Ssd1306::fill(l4_uint8_t pattern)
{
  memset(_fb, pattern, Fb_size);
}

void Ssd1306::set_contrast(l4_uint8_t val)
{
  l4_uint8_t cmds[] = { 0x81, val };
  send_cmd_seq(cmds, sizeof(cmds));
}

void Ssd1306::invert_display(bool inv)
{
  send_cmd(inv ? 0xA7 : 0xA6);
}

void Ssd1306::display_on(bool on)
{
  send_cmd(on ? 0xAF : 0xAE);
}

void Ssd1306::set_cursor(unsigned x, unsigned y)
{
  _cursor_x = x;
  _cursor_y = y;
}

void Ssd1306::draw_char(char c)
{
  unsigned char ch = static_cast<unsigned char>(c);
  if (ch < Font_first_char || ch > Font_last_char)
    ch = Font_first_char; // render space for unprintable

  unsigned idx = ch - Font_first_char;

  // Each character occupies one page (8 vertical pixels)
  unsigned page = _cursor_y / 8;
  if (page >= Pages)
    return;

  for (unsigned col = 0; col < Font_char_width; ++col)
    {
      if (_cursor_x + col < Width)
        _fb[page * Width + _cursor_x + col] |= font5x7[idx][col];
    }

  // 1-pixel gap between characters
  _cursor_x += Font_char_width + 1;

  // Wrap to next line
  if (_cursor_x + Font_char_width > Width)
    {
      _cursor_x = 0;
      _cursor_y += 8;
    }
}

void Ssd1306::draw_string(char const *s)
{
  while (*s)
    draw_char(*s++);
}
