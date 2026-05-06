-- vi:ft=lua
-- IO hardware config for i.MX6UL
-- i.MX6UL TRM: UART1 @ 0x02020000, SPI IRQ 26 (GIC: 32+26=58)

Io.hw_add_devices(function()
  -- UART1 (console, 115200 8N1)
  uart1 = Io.Hw.Device(function()
    compatible = { "fsl,imx6ul-uart", "fsl,imx6q-uart" };
    Resource.reg0 = Io.Res.mmio(0x02020000, 0x02023fff);
    Resource.irq0 = Io.Res.irq(32 + 26, Io.Resource.Irq_type_level_high);
  end)
end)

local hw = Io.system_bus()

Io.add_vbusses {
  vbus_uart = Io.Vi.System_bus(function()
    UART1 = wrap(hw:match("fsl,imx6ul-uart"))
  end),
}
