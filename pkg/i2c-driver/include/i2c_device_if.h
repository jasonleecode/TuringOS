/*
 * Copyright (C) 2024-2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/sys/kobject>
#include <l4/sys/cxx/ipc_epiface>


struct I2c_device_ops
: public L4::Kobject_t<I2c_device_ops, L4::Kobject, L4::PROTO_ANY,
                       L4::Type_info::Demand_t<1>>
{
  /**
   * Read data from the i2c device.
   *
   * \param len       Amount of data to read.
   * \param buf[out]  Buffer to read data into.
   *
   * \retval L4_EOK  Sucessfully read `len` bytes from i2c device.
   * \retval <0      Error or less data read.
   */
  L4_INLINE_RPC(long, read, (unsigned char len,
                             L4::Ipc::Array<l4_uint8_t> &buf));

  /**
   * Write all bytes in `data` to the given i2c device.
   *
   * \param data  Buffer containing data to write.
   *
   * \retval L4_EOK  Sucessfully wrote all bytes to i2c device.
   * \retval <0      Error or not all bytes written.
   */
  L4_INLINE_RPC(long, write, (L4::Ipc::Array<l4_uint8_t const> data));

  /**
   * Combined write to and read from the i2c device.
   *
   * \param data      Data buffer to write to the device.
   * \param len       Amount of data to read.
   * \param buf[out]  Buffer to read data into.
   *
   * \retval L4_EOK  Sucessfully written and read all data to and from the
   *                 i2c device.
   * \retval <0      Error case.
   *
   * The usual interaction with an i2c device is to write the register to read
   * to the i2c device and then read a certain amount of data. Do this in one
   * call to the i2c controller.
   */
  L4_INLINE_RPC(long, write_read,
                (L4::Ipc::Array<l4_uint8_t const> data, unsigned char len,
                 L4::Ipc::Array<l4_uint8_t> &buf));

//  L4_INLINE_RPC(long, read_dma, (l4_addr_t, unsigned));
//  L4_INLINE_RPC(long, write_dma, (l4_addr_t, unsigned));

  using Rpcs = L4::Typeid::Rpcs<read_t, write_t, write_read_t>;
};

