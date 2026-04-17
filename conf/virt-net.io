-- QEMU ARM virt  –  virtio-net MMIO device declaration
--
-- QEMU places virtio-mmio devices at 0xa000000 + 0x200 * slot.
-- The first device (slot 0) gets:
--   MMIO: 0x0a000000 .. 0x0a0001ff  (512 bytes)
--   IRQ:  GIC SPI 16  →  io resource irq(32 + 16) = 48
--
-- QEMU command-line to activate:
--   -netdev user,id=net0,hostfwd=tcp::5555-:5000
--   -device virtio-net-device,netdev=net0

Io.hw_add_devices(function()
  virtio_net = Io.Hw.Device(function()
    compatible = { "virtio,mmio" };
    Resource.reg0 = Io.Res.mmio(0x0a000000, 0x0a0001ff);
    Resource.irq0 = Io.Res.irq(32 + 16, Io.Resource.Irq_type_raising_edge);
  end)
end)

local hw = Io.system_bus()

Io.add_vbusses {
  vbus_net = Io.Vi.System_bus(function()
    NET = wrap(hw:match("virtio,mmio"))
  end)
}
