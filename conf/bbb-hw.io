-- vi:ft=lua
-- IO hardware configuration for BeagleBone Black (AM335x)
-- Basic UART, GPIO, I2C setup for native_shell devices

Io.hw_add_devices(function()
  -- UART0 for console (OMAP3 UART)
  uart0 = Io.Hw.Device(function()
    compatible = { "ti,omap3-uart" };
    Resource.reg0 = Io.Res.mmio(0x44e09000, 0x44e09fff);
    Resource.irq0 = Io.Res.irq(74, Io.Resource.Irq_type_level_high);
  end)

  -- GPIO bank 1 for DS18B20 temperature sensor
  gpio1 = Io.Hw.Device(function()
    compatible = { "ti,omap3-gpio" };
    Resource.reg0 = Io.Res.mmio(0x4804c000, 0x4804cfff);
    Resource.irq0 = Io.Res.irq(96, Io.Resource.Irq_type_level_high);
  end)

  -- I2C1 for TEF6686HN radio
  i2c1 = Io.Hw.Device(function()
    compatible = { "ti,omap3-i2c" };
    Resource.reg0 = Io.Res.mmio(0x4802a000, 0x4802afff);
    Resource.irq0 = Io.Res.irq(71, Io.Resource.Irq_type_level_high);
  end)
end)

local hw = Io.system_bus()

-- Create vbus for GPIO/I2C devices
Io.add_vbusses {
  vbus_hw = Io.Vi.System_bus(function()
    GPIO = wrap(hw:match("ti,omap3-gpio"))
    I2C  = wrap(hw:match("ti,omap3-i2c"))
  end)
}