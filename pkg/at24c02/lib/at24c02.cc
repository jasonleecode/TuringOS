/*
 * AT24C02 I2C EEPROM driver implementation.
 *
 * Protocol summary (from Atmel/Microchip AT24C02 datasheet):
 *
 *  Random Read:
 *    [START][SLA+W][word_addr][START][SLA+R][data][STOP]
 *    → i2c write_read({word_addr}, 1)
 *
 *  Sequential Read (n bytes):
 *    [START][SLA+W][word_addr][START][SLA+R][d0]...[dN][STOP]
 *    → i2c write_read({word_addr}, n)
 *
 *  Byte Write:
 *    [START][SLA+W][word_addr][data][STOP]  + 5 ms write cycle
 *    → i2c write({word_addr, data})
 *
 *  Page Write (up to 8 bytes, must not cross page boundary):
 *    [START][SLA+W][word_addr][d0]...[d7][STOP]  + 5 ms write cycle
 *    → i2c write({word_addr, d0, ..., dN})
 */

#include <l4/at24c02/at24c02.h>
#include <l4/util/util.h>   /* l4_usleep */
#include <l4/sys/err.h>
#include <l4/sys/l4int.h>

#include <cstring>

At24c02::At24c02(L4::Cap<I2c_device_ops> i2c)
: _i2c(i2c)
{}

long
At24c02::read_byte(l4_uint8_t addr, l4_uint8_t *data)
{
  return read(addr, data, 1);
}

long
At24c02::read(l4_uint8_t addr, l4_uint8_t *buf, unsigned len)
{
  if (!buf || len == 0)
    return -L4_EINVAL;

  /* Clamp to address space */
  if (addr + len > Capacity)
    len = Capacity - addr;

  /* write_read: send the word address, then clock in len bytes */
  l4_uint8_t wbuf[1] = { addr };
  L4::Ipc::Array<l4_uint8_t const> wr(1, wbuf);
  L4::Ipc::Array<l4_uint8_t>       rb(len, buf);

  return _i2c->write_read(wr, static_cast<unsigned char>(len), rb);
}

long
At24c02::write_page(l4_uint8_t addr, l4_uint8_t const *data, unsigned len)
{
  /*
   * Build a single I2C write message: [word_addr][d0]..[dN-1]
   * Maximum payload is 1 (address) + Page_size bytes.
   */
  l4_uint8_t buf[1 + Page_size];
  buf[0] = addr;
  memcpy(buf + 1, data, len);

  L4::Ipc::Array<l4_uint8_t const> wr(1 + len, buf);
  return _i2c->write(wr);
}

long
At24c02::write_byte(l4_uint8_t addr, l4_uint8_t data)
{
  long r = write_page(addr, &data, 1);
  if (r >= 0)
    l4_usleep(Write_cycle_us);
  return r;
}

long
At24c02::write(l4_uint8_t addr, l4_uint8_t const *data, unsigned len)
{
  if (!data || len == 0)
    return -L4_EINVAL;

  /* Clamp to address space */
  if (addr + len > Capacity)
    len = Capacity - addr;

  unsigned written = 0;
  while (written < len)
    {
      l4_uint8_t cur_addr = addr + written;

      /* Bytes remaining in the current hardware page */
      unsigned page_off      = cur_addr % Page_size;
      unsigned bytes_in_page = Page_size - page_off;

      unsigned chunk = len - written;
      if (chunk > bytes_in_page)
        chunk = bytes_in_page;

      long r = write_page(cur_addr, data + written, chunk);
      if (r < 0)
        return r;

      /* Wait for internal write cycle to complete */
      l4_usleep(Write_cycle_us);

      written += chunk;
    }

  return L4_EOK;
}
