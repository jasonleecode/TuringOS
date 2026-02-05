/*
 * SPI device RPC interface.
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_epiface>


struct Spi_device_ops
: public L4::Kobject_t<Spi_device_ops, L4::Kobject, L4::PROTO_ANY,
                       L4::Type_info::Demand_t<1>>
{
  /**
   * Full-duplex SPI transfer.
   *
   * \param      tx_buf  Data to transmit (MOSI).
   * \param      rx_len  Number of bytes to receive.
   * \param[out] rx_buf  Buffer to receive data into (MISO).
   *
   * \retval L4_EOK  Successfully transferred data.
   * \retval <0      Error during transfer.
   */
  L4_INLINE_RPC(long, transfer, (L4::Ipc::Array<l4_uint8_t const> tx_buf,
                                 unsigned short rx_len,
                                 L4::Ipc::Array<l4_uint8_t> &rx_buf));

  /**
   * Read data from SPI device (MISO only).
   *
   * \param      len       Amount of data to read.
   * \param[out] buf       Buffer to read data into.
   *
   * \retval L4_EOK  Successfully read `len` bytes.
   * \retval <0      Error or less data read.
   */
  L4_INLINE_RPC(long, read, (unsigned short len,
                             L4::Ipc::Array<l4_uint8_t> &buf));

  /**
   * Write data to SPI device (MOSI only).
   *
   * \param data  Buffer containing data to write.
   *
   * \retval L4_EOK  Successfully wrote all bytes.
   * \retval <0      Error or not all bytes written.
   */
  L4_INLINE_RPC(long, write, (L4::Ipc::Array<l4_uint8_t const> data));

  /**
   * Configure SPI device parameters.
   *
   * \param mode      SPI mode (0-3): CPOL/CPHA configuration.
   * \param speed_hz  Clock frequency in Hz.
   *
   * \retval L4_EOK  Successfully configured.
   * \retval <0      Error during configuration.
   */
  L4_INLINE_RPC(long, configure, (l4_uint8_t mode, l4_uint32_t speed_hz));

  using Rpcs = L4::Typeid::Rpcs<transfer_t, read_t, write_t, configure_t>;
};
