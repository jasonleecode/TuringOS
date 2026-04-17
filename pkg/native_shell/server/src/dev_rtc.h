// /dev/rtc0 — Real-time clock.
// read()  → "2026-04-17 14:30:00 UTC\n"
// write() → "2026-04-17 14:30:00\n" sets the clock
#pragma once

#include <l4/devfs/device_file.h>

class Rtc_device_file : public Devfs::Device_file
{
public:
  ssize_t readv(const struct iovec *iov, int iovcnt) noexcept override;
  ssize_t writev(const struct iovec *iov, int iovcnt) noexcept override;

  ~Rtc_device_file() noexcept override {}
};
