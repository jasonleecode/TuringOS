-- QEMU ARM virt  –  VirtIO block device MMIO declarations
--
-- QEMU fills virtio-mmio slots from the top (slot 31) downward unless the
-- bus is explicitly specified.  run_qemu_virt.sh pins devices to fixed slots:
--   Slot 0  virtio-net  0x0a000000  IRQ 48  (GIC SPI 16)  [bus=virtio-mmio-bus.0]
--   Slot 1  virtio-blk  0x0a000200  IRQ 49  (GIC SPI 17)  [bus=virtio-mmio-bus.1]
--
-- The driver filters at runtime by reading the DeviceID register (net=1, blk=2).
-- If --no-net is used, slot 0 is empty (DeviceID=0) and the driver skips it.
--
-- QEMU command line (added by run_qemu_virt.sh --disk):
--   -drive if=none,id=vdisk,file=build/virt_disk.img,format=raw
--   -device virtio-blk-device,drive=vdisk,bus=virtio-mmio-bus.1

Io.hw_add_devices(function()
  -- Slot 0: 0x0a000000-0x0a0001ff, GIC SPI 16 (irq 48)
  SLOT0 = Io.Hw.Device(function()
    compatible = { "virtio,mmio" };
    Resource.reg0 = Io.Res.mmio(0x0a000000, 0x0a0001ff);
    Resource.irq0 = Io.Res.irq(32 + 16, Io.Resource.Irq_type_raising_edge);
  end)

  -- Slot 1: 0x0a000200-0x0a0003ff, GIC SPI 17 (irq 49)
  SLOT1 = Io.Hw.Device(function()
    compatible = { "virtio,mmio" };
    Resource.reg0 = Io.Res.mmio(0x0a000200, 0x0a0003ff);
    Resource.irq0 = Io.Res.irq(32 + 17, Io.Resource.Irq_type_raising_edge);
  end)
end)

local hw = Io.system_bus()

-- Expose all virtio,mmio devices on one vbus; driver picks block device by DeviceID
Io.add_vbusses {
  vbus_blk = Io.Vi.System_bus(function()
    DEVS = wrap(hw:match("virtio,mmio"))
  end)
}
