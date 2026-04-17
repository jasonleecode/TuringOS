// /dev/temp0 — DS18B20 temperature sensor via GPIO/vbus.
// read() returns "+24.35\n" (temperature in Celsius).
#pragma once

#include <l4/devfs/device_file.h>
#include <l4/ds18b20/ds18b20.h>

class Temp_device_file : public Devfs::Device_file
{
public:
  explicit Temp_device_file(L4vbus::Gpio_pin pin) : _sensor(pin) {}

  ssize_t readv(const struct iovec *iov, int iovcnt) noexcept override;

  ~Temp_device_file() noexcept override {}

private:
  Ds18b20 _sensor;
};
