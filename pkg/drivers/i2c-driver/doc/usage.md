# I2C controller driver {#l4re_servers_i2c_driver}

[comment]: # (This is a generated file. Do not change it.)
[comment]: # (Instead, change capdb.yml.)

The I2C controller driver provides means to separate access to devices on a
shared I2C bus to multiple clients. The assignment is static and assigned during
the system configuration (ned script).

The vBus passed to the I2C controller driver shall only contain a single I2C
controller.

### Supported hardware I2C controllers
   - BCM2835, BCM2711


## Capabilities

* `dev_factory`

  The capability to the factory interface channel must be named `dev_factory` in
  the capability table of the I2C controller driver.

  Mandatory capability.


## Command Line Options

I2C controller driver provides the following command line options:

* `-v`, `--verbose`

  Increase the verbosity level by one. The supported verbosity levels are Quiet,
  Warn, Info, and Trace. The default verbosity level is Warn.

  Can be used up to 3 times.

  Flag. True if provided.

* `-q`, `--quiet`

  Silence all output of the I2C controller driver.

  Flag. True if provided.

## Virtio I2C device

The virtio-i2c-device interface allows for direct usage as a virtIO device. For
example with uvmm's virtio-proxy a guest can directly access the device(s).

Call:   `create(0, "addr=<0xYY>")`

* `"addr=<0xYY>"`

  I2C device address in hex number format (`"addr=0xYY"`) used to identify this
  device.

  Hexadecimal number with '0x' prefix.

Each i2c device address can only be used once, each subsequent create call will
return `-L4_EEXIST`.


## I2C device

The i2c-device interface allows usage of an I2C device from an L4Re I2C driver
via the RPC interface.

Call:   `create(1, "addr=<0xYY>")`

* `"addr=<0xYY>"`

  I2C device address in hex number format (`"addr=0xYY"`) used to identify this
  device.

  Hexadecimal number with '0x' prefix.

Each i2c device address can only be used once, each subsequent create call will
return `-L4_EEXIST`.



## Examples

### usage example for DS321 RTC device
```lua
local i2c_vbus = ld:new_channel()
-- Connect `i2c_vbus` to the vBus containing one i2c controller at `io`.

local i2c_drv_fab = ld:new_channel()

ld:start(
  {
    log = {"i2cdrv", "g"},
    caps =
    {
      dev_factory = i2c_drv_fab:svr(),
      vbus = i2c_vbus,
      jdb = L4.Env.jdb
    }
  }, "rom/i2c-driver")

 -- 0: create a type 0 frontend: virtio-i2c-device
 -- 0x68: I2C address of the DS3231 RTC device
local i2c_rtc = i2c_drv_fab:create(0, "addr=0x68")

-- The i2c_rtc capability can be passed as named cap to a virtio-proxy
-- device in uvmm.
```

### device tree snippet for an virtio-i2c-device to use with uvmm

This snippet can be used in a uvmm device tree. The `virtio_i2c` device node
contains an `i2c` node, describing the bus with the `rtc@68` node as only device
on this bus. The `0x68` value is the bus address of the DS3231 RTC device and
`i2c_rtc` is the name of the capability to the i2c-driver.

```dts
virtio_i2c@16000 {
  compatible = "virtio,mmio";
  reg = <0 0xf1116000 0 0x200>;
  interrupts = <0 129 4>;
  l4vmm,vdev = "proxy";
  l4vmm,virtiocap = "i2c_rtc";
  status = "okay";

  i2c {
    compatible = "virtio,device22";

    #size-cells = <1>;
    #address-cells = <0>;

    rtc@68 {
      compatible = "maxim,ds3231";
      reg = <0x68>;
    };
  };
};
```

