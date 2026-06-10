-- vi:ft=lua
-- IO hardware config for QEMU ARM virt: pl031 RTC + VirtIO MMIO slots 0 & 1
--
--   pl031 RTC:  MMIO 0x9010000, SPI IRQ 2  (level-high)
--   Slot 0 net: MMIO 0xa000000, SPI IRQ 16 (raising-edge)
--   Slot 1 blk: MMIO 0xa000200, SPI IRQ 17 (raising-edge)

Io.hw_add_devices(function()
  -- PL031 RTC
  pl031 = Io.Hw.Device(function()
    compatible = { "arm,pl031", "arm,primecell" };
    Resource.reg0 = Io.Res.mmio(0x9010000, 0x9010fff);
    Resource.irq0 = Io.Res.irq(32 + 2, Io.Resource.Irq_type_level_high);
  end)

  -- VirtIO-MMIO slot 0 (virtio-net, bus=virtio-mmio-bus.0)
  SLOT0 = Io.Hw.Device(function()
    compatible = { "virtio,mmio" };
    Resource.reg0 = Io.Res.mmio(0x0a000000, 0x0a0001ff);
    Resource.irq0 = Io.Res.irq(32 + 16, Io.Resource.Irq_type_raising_edge);
  end)

  -- VirtIO-MMIO slot 1 (virtio-blk, bus=virtio-mmio-bus.1)
  SLOT1 = Io.Hw.Device(function()
    compatible = { "virtio,mmio" };
    Resource.reg0 = Io.Res.mmio(0x0a000200, 0x0a0003ff);
    Resource.irq0 = Io.Res.irq(32 + 17, Io.Resource.Irq_type_raising_edge);
  end)

  -- VirtIO-MMIO slot 2 (virtio-serial, bus=virtio-mmio-bus.2)
  SLOT2 = Io.Hw.Device(function()
    compatible = { "virtio,mmio", "turingos,uart" };
    Resource.reg0 = Io.Res.mmio(0x0a000400, 0x0a0005ff);
    Resource.irq0 = Io.Res.irq(32 + 18, Io.Resource.Irq_type_raising_edge);
  end)

  -- VirtIO-MMIO slot 3 (virtio-keyboard, bus=virtio-mmio-bus.3)
  SLOT3 = Io.Hw.Device(function()
    compatible = { "virtio,mmio", "turingos,input" };
    Resource.reg0 = Io.Res.mmio(0x0a000600, 0x0a0007ff);
    Resource.irq0 = Io.Res.irq(32 + 19, Io.Resource.Irq_type_raising_edge);
  end)

  -- VirtIO-MMIO slot 4 (virtio-tablet, bus=virtio-mmio-bus.4)
  SLOT4 = Io.Hw.Device(function()
    compatible = { "virtio,mmio", "turingos,input" };
    Resource.reg0 = Io.Res.mmio(0x0a000800, 0x0a0009ff);
    Resource.irq0 = Io.Res.irq(32 + 20, Io.Resource.Irq_type_raising_edge);
  end)
end)

local hw = Io.system_bus()

Io.add_vbusses {
  -- RTC server gets the pl031 device
  vbus_rtc = Io.Vi.System_bus(function()
    RTC = wrap(hw:match("arm,pl031"))
  end),

  -- Block driver gets only the VirtIO block device (SLOT1)
  vbus_blk = Io.Vi.System_bus(function()
    BLK = wrap(hw.SLOT1)
  end),

  -- Input consumer gets only keyboard + tablet (turingos,input compatible)
  vbus_input = Io.Vi.System_bus(function()
    DEVS = wrap(hw:match("turingos,input"))
  end),

  -- ds18b20-server gets a GPIO bus.  QEMU virt has no GPIO controller wired
  -- here, so this bus is empty and the server falls back to simulated readings.
  -- On real hardware (e.g. BBB) add the GPIO device to this bus.
  vbus_gpio = Io.Vi.System_bus(function()
  end),
}
