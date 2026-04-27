-- vi:ft=lua
-- IO hardware config for uart-test: only SLOT2 (virtio-serial, bus=virtio-mmio-bus.2)
--   MMIO 0xa000400, SPI IRQ 18 (raising-edge)

Io.hw_add_devices(function()
  SLOT2 = Io.Hw.Device(function()
    compatible = { "virtio,mmio", "turingos,uart" };
    Resource.reg0 = Io.Res.mmio(0x0a000400, 0x0a0005ff);
    Resource.irq0 = Io.Res.irq(32 + 18, Io.Resource.Irq_type_raising_edge);
  end)
end)

local hw = Io.system_bus()

Io.add_vbusses {
  vbus_serial = Io.Vi.System_bus(function()
    DEV = wrap(hw:match("virtio,mmio"))
  end),
}
