/*
 * DS18B20 temperature bridge for native shell.
 *
 * Exposes a plain C function so commands.c can call the C++ driver.
 * Requires a "vbus" capability containing a GPIO device connected to
 * the DS18B20 DQ line (4.7 kΩ pull-up to VCC).
 */

#include <l4/ds18b20/ds18b20.h>
#include <l4/re/env>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_gpio>
#include <cstdio>

extern "C" int ds18b20_read_temp(int pin, int *temp_c100);

int ds18b20_read_temp(int pin, int *temp_c100)
{
    auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
    if (!vbus)
    {
        printf("temp: 'vbus' capability not available\n");
        return -1;
    }

    L4vbus::Device gpio_dev;
    if (vbus->root().device_by_hid(&gpio_dev, "gpio") < 0)
    {
        printf("temp: GPIO device not found on vbus\n");
        return -1;
    }

    L4vbus::Gpio_pin dq(gpio_dev, pin);
    Ds18b20 sensor(dq);

    if (!sensor.present())
    {
        printf("temp: no device detected on GPIO pin %d\n", pin);
        return -1;
    }

    if (sensor.read_temp_c100(temp_c100) != L4_EOK)
    {
        printf("temp: read failed (bus error or CRC mismatch)\n");
        return -1;
    }

    return 0;
}
