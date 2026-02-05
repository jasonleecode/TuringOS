# L4Re USB driver

This package provides an initial USB driver for L4Re.
It currently supports OHCI and HID with keyboard and pointing devices

## Structure
```
examples/
  ledfan/           - driver for an usb-programmable ledfan (Microdia XY LED FAN) 

examples/assets/
  usb.ned           - sample config for Ned
  usb.io            - L4vbus config with Host-Controllers

hid/                - driver for HID devices, currently supporting keyboard and pointing devices

host/               - host controller drivers
  include/          - definitions used in both server and clients, RPC definitions
  src/              - the USB Server 

doc/
  classes.svg       - Class Diagram 
  sequence.svg      - Sequence Diagram 
```



## Notes for QEMU/VBox/KVM

### qemu usb-dev passthrough without root
```shell
nano /etc/udev/rules.d/10-qemu-hw-users.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="145f", ATTRS{idProduct}=="01b4", TAG+="uaccess"
# or
sudo chmod -R 777 /dev/bus/usb
```

### vbox usb-dev passthrough without root
```shell
sudo adduser $USER vboxusers
# or
sudo usermod -aG vboxusers $USER
```

### qemu commandline args in kvm
```xml
<domain xmlns:qemu="http://libvirt.org/schemas/domain/qemu/1.0" type="kvm">
  ...
  <qemu:commandline>
    <qemu:arg value="-d"/>
    <qemu:arg value="guest_errors"/>
    <qemu:arg value="--trace"/>
    <qemu:arg value="usb_ohci_*"/>
  </qemu:commandline>
```

### ohci-emulation via kvm
```xml
    <controller type="usb" index="0" model="qemu-xhci" ports="15">
    	<address type="pci" domain="0x0000" bus="0x00" slot="0x03" function="0x0"/>
    </controller>
    <controller type="usb" index="1" model="pci-ohci">
      <address type="pci" domain="0x0000" bus="0x00" slot="0x09" function="0x0"/>
    </controller>
```

### pci passthrough via kvm
```xml
  <hostdev mode="subsystem" type="pci" managed="yes">
      <source>
        <!-- 00:12.0 USB controller: -->
        <address domain="0x0000" bus="0x00" slot="0x12" function="0x0"/>
      </source>
      <rom bar="off"/>
      <address type="pci" domain="0x0000" bus="0x00" slot="0x06" function="0x0"/>
    </hostdev>
```

### binding vfio-pci to pci-devices
```shell
lspci -nnv -s 00:12.0
# -> Kernel driver in use: ohci-pci

sudo virsh nodedev-detach pci_0000_00_12_0
sudo virsh nodedev-detach pci_0000_00_12_2

lspci -nnv -s 00:12.0
# -> Kernel driver in use: vfio-pci

sudo virsh nodedev-reattach pci_0000_00_12_0
sudo virsh nodedev-reattach pci_0000_00_12_2


# alternatively via sysfs

echo "0000:00:12.0" | sudo tee /sys/bus/pci/devices/0000:00:12.0/driver/unbind
echo "0000:00:12.2" | sudo tee /sys/bus/pci/devices/0000:00:12.2/driver/unbind
echo "vfio-pci" | sudo tee /sys/bus/pci/devices/0000:00:12.0/driver_override
echo "vfio-pci" | sudo tee /sys/bus/pci/devices/0000:00:12.2/driver_override
echo "0000:00:12.0" | sudo tee /sys/bus/pci/drivers_probe
echo "0000:00:12.2" | sudo tee /sys/bus/pci/drivers_probe
lspci -nnv -s 00:12.0

echo "0000:00:12.0" | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind
echo "ohci-pci" | sudo tee /sys/bus/pci/devices/0000:00:12.0/driver_override
echo "0000:00:12.0" | sudo tee /sys/bus/pci/drivers_probe
```

### QEMU command line options

Useful command line options for Makeconf.boot file

```make
QEMU_OPTIONS   += -device pci-ohci,id=ohci
#QEMU_OPTIONS   += -usb -device pci-ohci,id=ohci -device usb-ehci,id=ehci -device qemu-xhci,id=xhci
QEMU_OPTIONS   += -device usb-mouse,bus=ohci.0,pcap=usb-driver.pcap
#QEMU_OPTIONS   += -device usb-kbd,bus=ohci.0,pcap=usb-driver.pcap
#QEMU_OPTIONS   += -device usb-tablet,bus=ohci.0,pcap=usb-driver.pcap
#QEMU_OPTIONS   += -device usb-host,bus=ohci.0,vendorid=0x0738,productid=0x1704,pcap=usb-driver.pcap
#QEMU_OPTIONS   += -drive if=none,id=test_img,format=raw,file=test_img.img -device usb-storage,drive=test_img
```

