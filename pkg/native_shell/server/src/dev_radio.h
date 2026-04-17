// /dev/radio0 — TEF6686HN FM/AM tuner.
// read()  → JSON status line: {"band":"FM","freq":96500,"rssi":-45}\n
// write() → command string:
//             "init\n"
//             "tune FM 96500\n"  (or MW/LW)
//             "seek up\n"  / "seek down\n"
//             "mute\n"     / "unmute\n"
//             "vol 150\n"
#pragma once

#include <l4/devfs/device_file.h>
#include <l4/tef6686hn/tef6686hn.h>

class Radio_device_file : public Devfs::Device_file
{
public:
  explicit Radio_device_file(L4::Cap<I2c_device_ops> i2c) : _radio(i2c) {}

  ssize_t readv(const struct iovec *iov, int iovcnt) noexcept override;
  ssize_t writev(const struct iovec *iov, int iovcnt) noexcept override;

  ~Radio_device_file() noexcept override {}

private:
  Tef6686hn _radio;
  bool      _inited = false;
};
