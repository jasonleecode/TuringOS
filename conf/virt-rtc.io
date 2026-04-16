-- vi:ft=lua
-- IO hardware configuration for QEMU ARM virt platform
-- Exposes the pl031 RTC device to the RTC server via a virtual bus.
-- pl031 is at 0x9010000, size 0x1000, SPI IRQ 2 (arm,pl031 from QEMU virt DTB).

Io.hw_add_devices(function()
  pl031 = Io.Hw.Device(function()
    compatible = { "arm,pl031", "arm,primecell" };
    Resource.reg0 = Io.Res.mmio(0x9010000, 0x9010fff);
    Resource.irq0 = Io.Res.irq(32 + 2, Io.Resource.Irq_type_level_high);
  end)
end)

local hw = Io.system_bus()

Io.add_vbusses {
  vbus_rtc = Io.Vi.System_bus(function()
    RTC = wrap(hw:match("arm,pl031"))
  end)
}
