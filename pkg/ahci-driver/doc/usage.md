# AHCI driver {#l4re_servers_ahci_driver}

[comment]: # (This is a generated file. Do not change it.)
[comment]: # (Instead, change capdb.yml.)

The AHCI driver is a driver for PCI serial ATA host controllers.

The AHCI driver is capable of exposing entire disks (by serial number) or
individual partitions (by their partition UUID) of a hard drive to clients via
the Virtio block interface.

The driver consists of two parts. The first one is the hardware driver itself
that takes care of the communication with the underlying hardware and interrupt
handling. The second part implements a virtual block device and is responsible
to communicate with clients. The virtual block device translates commands it
receives into AHCI requests and issues them to the hardware driver.

The AHCI driver allows both statically and dynamically configured clients. A
static configuration is given priority over dynamically connecting clients and
configured while the service starts. Dynamic clients can connect and disconnect
during runtime of the AHCI driver.


## Capabilities

* `vbus`

  Virtual bus capability

  Mandatory capability.

* `client`

  Static client

  Multiple capability names can be provided by the `--client` command line
  parameter.

* `svr`

  Server Capability of application. Endpoint for IPC calls

  Mandatory capability.


## Command Line Options

In the example above the ahci driver is started in its default configuration. To
customize the configuration of the ahci-driver it accepts the following command
line options:

* `-A`, `--check-address`

  Disable check for address width of the device. Only do this if all physical
  memory is guaranteed to be below 4GB.

  Flag. True if provided.

* `-v`, `--verbose`

  Enable verbose mode. You can repeat this option up to three times to increase
  verbosity up to trace level.

  Can be used up to 3 times.

  Flag. True if provided.

* `-q`, `--quiet`

  This option enables the quiet mode. All output is silenced.

  Flag. True if provided.

* `--client <cap_name>`

  Connect a static client.

  Can be used multiple times.

  Name of a provided capability with server rights that adheres to the ipc
  protocol.

  This parameter opens a scope for the following subparameters:

  * `--device <<SN> | <SN>:<PARTNUM> | [partuuid:]<UUID> | [partlabel:]<LABEL>>`

    This option denotes either the AHCI device, or a partition on such a device
    to be exported for the client specified in the preceding `client` option.

    The AHCI device is specified via its serial number SN. The device serial
    number can be found e.g. in the AHCI server's output.

    The partition can be given either by the combination of the device's SN
    followed by a colon followed by the partition number PARTNUM, or by the
    partition's UUID or label. Both 'partuuid:' and 'partlabel:' strings are
    optional and help to disambiguate the device matching if necessary.

    String value.

  * `--ds-max <max>`

    This option sets the upper limit of the number of dataspaces the client is
    able to register with the AHCI driver for virtio DMA.

    Numerical value.

    Default: `2`

  * `--slot-max <max>`

    This option defines the maximum number of requests a single client may have
    in parallel running on the device. If a positive number is given, then this
    is considered the absolute number of slots to be used. If a negative number
    is given, then the client may use all available slots except the number
    given. In any case, a client gets at least 1 slot and at most the number of
    slots available in hardware. This parameter is only valid when a client
    accesses a partition and ignored otherwise.

    Numerical value.

  * `--readonly`

    This option sets the access to disks or partitions to read only for the
    preceding `client` option.

    Flag. True if provided.

## Building and Configuration

The AHCI driver can be built using the L4Re build system. Just place this
project into your `pkg` directory. The resulting binary is called `ahci-drv`

## Starting the service

The AHCI driver can be started with Lua like this:

```lua
local ahci_bus = L4.default_loader:new_channel();
L4.default_loader:start({
  caps = {
    vbus = vbus_ahci,
    svr = ahci_bus:svr(),
  },
},
"rom/ahci-drv");
```

First an IPC gate (`ahci_bus`) is created which is used between the AHCI driver
and a client to request access to a particular disk or partition. The server-
side is assigned to the mandatory `svr` capability of the AHCI driver. See the
section below on how to configure access to a disk or partition.

The ahci driver needs access to a virtual bus capability (`vbus`). On the
virtual bus the AHCI driver searches for AHCI 1.0 compliant storage controllers.
Please see io's documentation about how to setup a virtual bus.

## Virtio block host

Prior to connecting a client to a virtual block session it has to be created
using the following Lua function. It has to be called on the client side of the
IPC gate capability whose server side is bound to the ahci driver.

Call:   `create(0, "device=<<SN> | <SN>:<PARTNUM> | [partuuid:]<UUID> |
[partlabel:]<LABEL>>" [, "ds-max=<max>", "slot-max=<max>"])`

* `"device=<<SN> | <SN>:<PARTNUM> | [partuuid:]<UUID> | [partlabel:]<LABEL>>"`

  This string denotes either the AHCI device, or a partition on such a device
  that the client wants to be exported via the Virtio block interface.

  The AHCI device is specified via its serial number SN. The device serial
  number can be found e.g. in the AHCI server's output.

  The partition can be given either by the combination of the device's SN
  followed by a colon followed by the partition number PARTNUM, or by the
  partition's UUID or label. Both 'partuuid:' and 'partlabel:' strings are
  optional and help to disambiguate the device matching if necessary.

  String value.

* `"ds-max=<max>"`

  Specifies the upper limit of the number of dataspaces the client is allowed to
  register with the AHCI driver for virtio DMA.

  Numerical value.
    * In the range of 1 to 256 inclusive

  Default: `2`

* `"slot-max=<max>"`

  Specifies the maximum number of requests that will be processed in parallel by
  the AHCI device. See `--slot-max` option above for details.

  Numerical value.

If the `create()` call is successful a new capability which references an AHCI
virtio driver is returned. A client uses this capability to communicate with the
AHCI driver using the Virtio block protocol.



## Examples

A couple of examples on how to request different disks or partitions are listed
below.

* Request entire disk with the given serial number

Assume the AHCI server reported the following SN number (running in QEMU):

```
Serial number: <QM00005             >
```

A client can connect to this disk via:

```lua
vda = ahci_bus:create(0, "ds-max=5", "device=QM00005")
```

* Request a partition using a partition number

Assume the AHCI server reported the following SN number (running in QEMU):

```
Serial number: <QM00005             >
```

A client can connect to partition 2 on this device like this:

```lua
vda = ahci_bus:create(0, "ds-max=5", "device=QM00005:2"
```

* Request a partition with the given UUID

```lua
vda = ahci_bus:create(0, "ds-max=5", "device=88E59675-4DC8-469A-98E4-B7B021DC7FBE")
```

* Request a partition using a label

Assume there is a partition with label 'foobar'. A client can connect to it
using the following snippet:

```lua
vda = ahci_bus:create(0, "ds-max=5", "device=partlabel:foobar")
```

* A more elaborate example with a static client. The client uses the client side
of the `ahci_cl1` capability to communicate with the AHCI driver.

  ```lua
  local ahci_cl1 = L4.default_loader:new_channel();
  local ahci_bus = L4.default_loader:new_channel();
  L4.default_loader:start({
    caps = {
      vbus = vbus_ahci,
      svr = ahci_bus:svr(),
      cl1 = ahci_cl1:svr(),
    },
  },
  "rom/ahci-drv --client cl1 --device 88E59675-4DC8-469A-98E4-B7B021DC7FBE --ds-max 5");
  ```

