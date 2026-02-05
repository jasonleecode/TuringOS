local hw = Io.system_bus()

Io.add_vbusses
{
  vbus_usb = Io.Vi.System_bus
  {
    PCI0 = Io.Vi.PCI_bus
    {
      hci = wrap(hw:match("PCI/usb"));
      --uhci = wrap(hw:match("PCI/CC_0c0300"));
      --ohci = wrap(hw:match("PCI/CC_0c0310"));
      --ehci = wrap(hw:match("PCI/CC_0c0320"));
      --xhci = wrap(hw:match("PCI/CC_0c0330"));
    }
  };
}
