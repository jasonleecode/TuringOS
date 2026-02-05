# eMMC driver {#l4re_servers_emmc_driver}

[comment]: # (This is a generated file. Do not change it.)
[comment]: # (Instead, change capdb.yml.)

The eMMC driver is a driver for PCI Express eMMC controllers.

## Starting the service

The eMMC driver can be started with Lua like this:

```lua
local emmc_bus = L4.default_loader:new_channel();
L4.default_loader:start({
  caps = {
    vbus = vbus_emmc,
    svr = emmc_bus:svr(),
  },
}, "rom/emmc-drv");
```

First, an IPC gate (`emmc_bus`) is created which is used between the eMMC driver
and a client to request access to a particular disk or partition. The server
side is assigned to the mandatory `svr` capability of the eMMC driver. See the
section below on how to configure access to a disk or partition.

The eMMC driver needs access to a virtual bus capability (`vbus`). On the
virtual bus the eMMC driver searches for eMMC compliant storage controllers.
Please see io's documentation about how to setup a virtual bus.

### Supported devices

The eMMC driver supports SDHCI and SDHI controllers, in particular
- SDHI interfaces found on RCar3 r8a7795 boards
- SDHCI interfaces found on RPI4
- uSDHCI interfaces found on i.MX8 boards
- uSDHCI interfaces found on the S32G SoC
- the QEMU SD card emulation (SDHCI, see `doc/pcie-ecam.io`),
- the QEMU eMMC emulation (provided by extending the QEMU SD card emulation by
`doc/qemu-patch.diff`).


## Capabilities

* `vbus`

  Required for finding the device which should be driven by this driver.

  Mandatory capability.

* `client`

  Static client

  Multiple capability names can be provided by the `--client` command line
  parameter.

* `bbds`

  **Only used by the SDHCI driver.** Certain SDHCI devices cannot handle DMA
  requests with DMA buffers beyond 4GiB. The provided dataspace is used as
  bounce buffer if the driver detects that a certain request needs it. **Note:**
  The bounce buffer needs to be able to hold the memory for an entire read/write
  request. That means that the buffer is divided into the number of maximum
  segments (see `--max-seg` parameter).

* `sdhci_adma_buf`

  **Only used by the SDHCI driver.** Page (4096 bytes) for storing DMA
  descriptors for the SDHCI driver. If this capability is not provided, the
  driver will allocate an arbitrary page.

* `bcm2835_mbox_mem`

  **Only used by the SDHCI driver when attaching to an bcm2711-compatible
  device.** Page (4096 bytes) for storing bcm2835 mbox messages. The firmware
  mbox is used to perform voltage switching for certain SD card configurations.
  If this capability is not provided, the driver will allocate an arbitrary
  page.

* `svr`

  Server Capability of application. Endpoint for IPC calls

  Mandatory capability.


## Command Line Options

In the example above the eMMC driver is started in its default configuration. To
customize the configuration of the eMMC driver it accepts the following command
line options:

* `-v`, `--verbose`

  Enable verbose mode. You can repeat this option to increase verbosity up to
  trace level.

  Can be used up to 3 times.

  Flag. True if provided.

* `-q`, `--quiet`

  This option enables the quiet mode. All output is silenced.

  Flag. True if provided.

* `--disable-mode <mode>`

  This option allows to disable certain eMMC/SD card modes from autodetection.
  Modes with the `hs`-prefix are determined for eMMC devices, others are for SD
  card devices.

  Can be used multiple times.

  Possible values for `<mode>` are `hs26`, `hs52`, `hs52_ddr`, `hs200`, `hs400`,
  `sdr12`, `sdr25`, `sdr50`, `sdr104`, `ddr50`

* `--max-seg <max>`

  Maximum number of segments per request. This number is announced to the virtio
  interface and is also relevant for the required bounce buffer size, see below.

  Numerical value.

  Default: `64`

* `--client <cap_name>`

  Connect a static client.

  Can be used multiple times.

  Name of a provided capability with server rights that adheres to the ipc
  protocol.

  This parameter opens a scope for the following subparameters:

  * `--device <<PSN> | <PSN>:<PARTNUM> | [partuuid:]<UUID> |
  [partlabel:]<LABEL>>`

    This option denotes either the eMMC device, or a partition on such a device
    to be exported for the client specified in the preceding `client` option.

    The eMMC device is specified via its PSN (Product Serial Number). The PSN
    can be found e.g. in the eMMC server's output or in the device's sysfs
    directory in Linux.

    The partition can be given either by the combination of the device's PSN
    followed by a colon followed by the partition number PARTNUM, or by the
    partition's UUID or label. Both 'partuuid:' and 'partlabel:' strings are
    optional and help to disambiguate the device matching if necessary.

    String value.

  * `--ds-max <max>`

    This option sets the upper limit of the number of dataspaces the client is
    able to register with the eMMC driver for virtio DMA.

    Numerical value.

    Default: `2`

  * `--readonly`

    This option sets the access to disks or partitions to read only for the
    preceding `client` option.

    Flag. True if provided.

  * `--dma-map-all`

    Map the entire client dataspace into the DMA space at the first I/O request
    and never unmap the dataspace until the client is destroyed. This is the
    default behavior.

    Flag. True if provided.

  * `--dma-map-per-req`

    By default, the entire client dataspace is mapped into the DMA space at the
    first I/O request and the dataspace is never unmapped until the client is
    destroyed. Change the default behavior by mapping the relevant part of the
    dataspace before an I/O request and unmapping it after the request.

    Flag. True if provided.

## Virtio block host

Prior to connecting a client to a virtual block session it has to be created
using the following Lua function. It has to be called on the client side of the
IPC gate capability whose server side is bound to the eMMC driver.

Call:   `create(0, "device=<<PSN> | <PSN>:<PARTNUM> | [partuuid:]<UUID> |
[partlabel:]<LABEL>>" [, "ds-max=<max>", "readonly", "dma-map-all", "dma-map-
per-req"])`

* `"device=<<PSN> | <PSN>:<PARTNUM> | [partuuid:]<UUID> | [partlabel:]<LABEL>>"`

  This string denotes either the eMMC device, or a partition on such a device
  that the client wants to be exported via the Virtio block interface.

  The eMMC device is specified via its PSN (Product Serial Number). The PSN can
  be found e.g. in the eMMC server's output or in the device's sysfs directory
  in Linux.

  The partition can be given either by the combination of the device's PSN
  followed by a colon followed by the partition number PARTNUM, or by the
  partition's UUID or label. Both 'partuuid:' and 'partlabel:' strings are
  optional and help to disambiguate the device matching if necessary.

  String value.

* `"ds-max=<max>"`

  Specifies the upper limit of the number of dataspaces the client is allowed to
  register with the eMMC driver for virtio DMA.

  Numerical value.
    * Must be in the range of 1 to 256 inclusive.

  Default: `2`

* `"readonly"`

  This option sets the access to disks or partitions to read only for this
  client connection.

  Flag. True if provided.

* `"dma-map-all"`

  Map the entire client dataspace into the DMA space at the first I/O request
  and never unmap the dataspace until the client is destroyed. This is the
  default behavior.

  Flag. True if provided.

* `"dma-map-per-req"`

  By default, the entire client dataspace is mapped into the DMA space at the
  first I/O request and the dataspace is never unmapped until the client is
  destroyed. Change the default behavior by mapping the relevant part of the
  dataspace before an I/O request and unmapping it after the request.

  Flag. True if provided.

If the `create()` call is successful, a new capability which references an eMMC
virtio driver is returned. A client uses this capability to communicate with the
eMMC driver using the Virtio block protocol.



## Examples

A couple of examples on how to request different disks or partitions are listed
below.

* Request the whole eMMC device

Assume the eMMC server reported the following PSN number (running in QEMU):

```
eMMC-0[device]: product: 'QEMU!', manufactured 2/2006, mid=aa, psn=deadbeef
```

A client can connect to this disk via:

```lua
vda1 = emmc_bus:create(0, "ds-max=5", "device=deadbeef"
```

* Request a partition using a partition number

Assume the eMMC server reported the following PSN number (running in QEMU):

```
eMMC-0[device]: product: 'QEMU!', manufactured 2/2006, mid=aa, psn=deadbeef
```

A client can connect to partition 2 on this device like this:

```lua
vda1 = emmc_bus:create(0, "ds-max=5", "device=deadbeef:2"
```

* Request a partition with the given UUID

```lua
vda1 = emmc_bus:create(0, "ds-max=5", "device=partuuid:AFFA05B0-9379-480E-B9C6-5FF57FB1D194")
```

* Request a partition using a label

Assume there is a partition with label 'foobar'. A client can connect to it
using the following snippet:

```lua
vda1 = emmc_bus:create(0, "ds-max=5", "device=partlabel:foobar")
```

* A more elaborate example with a static client. The client uses the client side
of the `emmc_cl1` capability to communicate with the eMMC driver.

  ```lua
  local emmc_cl1 = L4.default_loader:new_channel();
  local emmc_bus = L4.default_loader:new_channel();
  L4.default_loader:start({
    caps = {
      vbus = vbus_emmc,
      svr = emmc_bus:svr(),
      cl1 = emmc_cl1:svr(),
    },
  }, "rom/emmc-drv --client cl1 --device 88E59675-4DC8-469A-98E4-B7B021DC7FBE --ds-max 5");
  ```

* Accessing a device from QEMU:

  The file `pcie-ecam.io` contains an IO config file which is able to use the
  QEMU PCI controller to search for attached eMMC devices.

* eMMC emulation with QEMU:

  The attached patch extends QEMU SD card emulation to emulate eMMC devices.
  After applying the patch and recompiling QEMU, attach the following parameters
  to your QEMU command line (assuming that `$HOME/foobar.img` is the eMMC
  medium):

  ```bash
  -drive id=sd_disk,file=$(HOME)/foobar.img,if=none,format=raw \
  -device sdhci-pci,id=sdhci \
  -device sd-card,drive=sd_disk,spec_version=3,emmc=on
  ```

