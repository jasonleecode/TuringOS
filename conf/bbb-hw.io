-- vi:ft=lua
-- IO hardware configuration for BeagleBone Black (AM335x)
-- Covers: UART0 (console), GPIO1 (DS18B20), I2C1 (TEF6686HN), RTC

Io.hw_add_devices(function()
  -- UART0 console (AM335x TRM: base 0x44E09000, IRQ 72)
  uart0 = Io.Hw.Device(function()
    compatible = { "ti,omap3-uart" };
    Resource.reg0 = Io.Res.mmio(0x44e09000, 0x44e09fff);
    Resource.irq0 = Io.Res.irq(72, Io.Resource.Irq_type_level_high);
  end)

  -- GPIO bank 1 (AM335x TRM: base 0x4804C000, IRQ 96)
  -- Used for DS18B20 one-wire temperature sensor
  gpio1 = Io.Hw.Device(function()
    compatible = { "ti,omap3-gpio" };
    Resource.reg0 = Io.Res.mmio(0x4804c000, 0x4804cfff);
    Resource.irq0 = Io.Res.irq(96, Io.Resource.Irq_type_level_high);
  end)

  -- I2C1 (AM335x TRM: base 0x4802A000, IRQ 71)
  -- Used for TEF6686HN FM radio chip
  i2c1 = Io.Hw.Device(function()
    compatible = { "ti,omap4-i2c", "ti,omap3-i2c" };
    Resource.reg0 = Io.Res.mmio(0x4802a000, 0x4802afff);
    Resource.irq0 = Io.Res.irq(71, Io.Resource.Irq_type_level_high);
  end)

  -- AM335x RTC (AM335x TRM: base 0x44E3E000, IRQ 75)
  rtc0 = Io.Hw.Device(function()
    compatible = { "ti,am3352-rtc", "ti,da830-rtc" };
    Resource.reg0 = Io.Res.mmio(0x44e3e000, 0x44e3efff);
    Resource.irq0 = Io.Res.irq(75, Io.Resource.Irq_type_level_high);
  end)
end)

local hw = Io.system_bus()

-- vbus_hw: GPIO1 + I2C1 for native_shell application drivers
Io.add_vbusses {
  vbus_hw = Io.Vi.System_bus(function()
    GPIO = wrap(hw:match("ti,omap3-gpio"))
    I2C  = wrap(hw:match("ti,omap4-i2c"))
  end),

  -- vbus_rtc: RTC for the rtc server
  vbus_rtc = Io.Vi.System_bus(function()
    RTC = wrap(hw:match("ti,am3352-rtc"))
  end),
}
