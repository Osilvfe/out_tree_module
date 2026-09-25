# Caihong non-camera sensors

This directory tracks ordinary sensor bring-up separately from the Qualcomm
camera stack.

## Architecture: the physical sensors are SSC-owned downstream

The OnePlus Pad Pro (`caihong`, project 23926) device tree includes
`sensor/caihong-sensor-23926.dtsi`.  That file does **not** describe physical
accelerometer, gyroscope, magnetometer or ALS devices on AP-visible I2C/SPI
buses.  It only creates the Oplus sensor-feedback SMEM endpoint and
`sensor-devinfo` node.

That matches the OnePlus `vendor/oplus/sensor` implementation: the Oplus kernel
code is an Android/Qualcomm sensor support layer (devinfo, tracing, virtual
sensors and command plumbing).  `sensor-devinfo` asks the sensor hub for
`CUST_ACTION_GET_SENSOR_INFO` and receives the physical device name from SSC;
it is not the physical sensor driver itself.

For mainline Linux the validated architecture is therefore:

1. recover the actual SSC registry hardware configuration;
2. run the upstream Hexagon FastRPC default listener for the ADSP sensors
   process;
3. serve the stock vendor and Caihong ODM registry configuration through a
   writable, persistent HexagonFS tree;
4. consume SSC data through `libssc`, while keeping direct-AP IIO drivers as
   explicit bus-handoff experiments only;
5. use the in-tree Linux input driver for the AP-visible GPIO Hall switch.

## Hardware recovered from Caihong vendor sensor registry

The public Caihong vendor image contains
`/odm/etc/sensor/config/json_list`.  Its hardware entries identify the active
sensor set below.

| Function | Registry hardware | Downstream bus / IRQ | Mainline plan |
| --- | --- | --- | --- |
| accelerometer + gyroscope | `icm4x607` | `bus_type=1` (SPI), instance 3, IRQ 80, high-level, keeper; orientation `-x -y +z` | validated through SSC; live accelerometer and gyroscope data, with the AP SPI node kept disabled |
| magnetometer | `mmc56x3x` | `bus_type=0` (I2C), instance 2, address 48 decimal (`0x30`), 100-400 kHz; orientation `+y -x +z` | validated through SSC for magnetometer and compass data; direct `mmc5633.ko` remains optional only |
| ALS / CCT | `tcs3701` through `sns_alsps` | I2C instance 2, address 57 decimal (`0x39`), IRQ 84 falling-edge, two sensor rails | live lux data validated through SSC; proximity remains unavailable; direct `tcs3701.ko` remains optional only |
| Hall / lid | `bu52053nvx` | SoC TLMM GPIO66, dual-edge, no pull, one `sensor_vddio` rail | in-tree `gpio-keys` exposes standard `EV_SW/SW_LID`; probe and suspend/resume validated |
| free-fall / flight-detect | virtual/algorithm configuration | built on physical sensor data | do not port until the underlying physical sensors work |
| barometer | not identified in the Caihong device-specific registry list | unknown | keep unresolved; do not guess a chip |

Qualcomm's SSC communication-port enum confirms registry `bus_type=0` is I2C
and `bus_type=1` is SPI.  Its interrupt enum confirms trigger type 1 is falling
edge, 2 is dual edge and 3 is high level.

## Upstream/adaptation policy

Prefer reviewed upstream Linux implementations over copying Android sensor-hub
code.  Linux v7.2 already contains the MMC5603/MMC5633 IIO driver; the local
`sensors/mmc5633.c` externalizes that implementation and intentionally keeps
only the I2C transport because Caihong's registry explicitly places this device
on I2C.

The upstream ICM42607 driver landed after Linux v7.2 and supports both
ICM42607/ICM42607P over I2C and SPI.  It remains the preferred IMU
implementation, but this tree does not guess whether Caihong's `icm4x607` is
ICM42607 (`WHO_AM_I=0x67`) or ICM42607P (`WHO_AM_I=0x60`).  The exact variant
must come from runtime or firmware evidence.

## TCS3701 direct-I2C baseline

Linux v7.2 has no TCS3701-specific IIO driver and the OnePlus kernel source does
not contain a physical TCS3701 AP driver; downstream ownership lives in the
Qualcomm sensor subsystem.  `sensors/tcs3701.c` therefore implements a small
standard-IIO baseline directly from the public ams OSRAM TCS3701 datasheet.

The baseline intentionally stays conservative:

- validates the documented device ID (`0x18`);
- exposes raw CLEAR, RED, GREEN and BLUE 16-bit ALS channels;
- exposes the raw 14-bit proximity result;
- programs the datasheet-recommended 50 ms ALS integration setup
  (`ASTEP=599`, `ATIME=29`);
- starts ALS at the documented 256x reset gain and allows the standard IIO
  calibration-scale control to select the documented 0.5x..1024x gain range;
- uses the datasheet characterization proximity setup (4 mA LED drive, 4x
  proximity gain, 8 us pulses, 8 pulses) as a low-power bring-up baseline;
- enables each measurement engine only for a direct read, so the first version
  does not depend on IRQ routing or runtime-PM state machines.

It does **not** yet claim production lux/CCT output or a calibrated near/far
threshold.  Those require the Caihong optical stack, panel/glass compensation,
factory calibration and board-specific IRQ/power validation.

## BU52053NVX Hall bring-up

The BU52053NVX is a GPIO-only switch, so Caihong uses the official in-tree
`gpio-keys` driver rather than a device-specific module:

- reads a GPIO only; there is no register bus for this Hall switch;
- reports `EV_SW/SW_LID` through the Linux input subsystem;
- handles both rising and falling edges as required by the Caihong SSC
  registry;
- supports wakeup.

`sensors/caihong-bu52053nvx-hall.dtsi` maps the confirmed TLMM GPIO66 and uses
`GPIO_ACTIVE_LOW`, matching the BU52053NVX output behavior.  The downstream
registry names its rail only as `/pmic/client/sensor_vddio`, so the DTS fragment
does not change its power controls.

Runtime validation on Caihong with Linux v7.2 confirmed that `gpio-keys`
claims GPIO66 with no pull, registers a dual-edge IRQ named `BU52053NVX Hall
Switch`, exposes the `SW_LID` capability, and remains wake-enabled after a
deep-sleep cycle.  The initial GPIO level was high and `SW_LID` was inactive,
as expected with no magnet present.  A physical cover/magnet transition is the
remaining event-path test.

## ICM42607 upstream driver port

The IMU module uses the official Linux `inv_icm42607` implementation from
commit `512d321b9397` (the complete upstream series, including the SPI front
end, accelerometer, gyroscope and temperature support).  It is built here as
three external modules:

- `inv_icm42607.ko` (shared core and IIO devices)
- `inv_icm42607_spi.ko` (SPI transport)
- `inv_icm42607_i2c.ko` (I2C transport)

The source is kept under `sensors/inv_icm42607/` without local protocol or
register changes.  Caihong's registry identifies the IMU as `icm4x607` on SSC
SPI instance 3 with a high-level IRQ and the orientation `-x -y +z`; the exact
ICM42607 versus ICM42607P identity and the AP-visible handoff still require
runtime evidence.  No DTS node is enabled by this module branch until that
handoff, chip-select, interrupt GPIO and `vdd`/`vddio` regulator mapping are
confirmed.

Build it with:

```sh
make -C sensors KDIR=/path/to/linux ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu-
```

The resulting modules only prove source/API compatibility.  Loading them on a
kernel whose DTS still assigns the same SPI controller to SSC is deliberately
unsupported.

## Qualcomm SSC runtime

The official SSC route is now functional. `hexagonrpcd` attaches to
`/dev/fastrpc-adsp` with `FASTRPC_IOCTL_INIT_ATTACH_SNS` and serves the stock
registry to the ADSP sensors process. The firmware requires registry writes;
using a read-only listener causes an ADSP fatal assertion. The pinned upstream
write-support series and Caihong mapping are documented in
[`ssc/README.md`](ssc/README.md).

Runtime testing produced stable accelerometer, gyroscope, magnetometer,
compass and ambient-light samples through `ssccli`. The ADSP remained running,
and its generated persistent registry files survived listener restart. This
confirms SSC SE3 and SE2 ownership, so the AP `spi3` and `i2c2` probe fragments
must remain opt-in and disabled in normal images.

## AP bus mapping and optional probe fragment

The vendor QUPv3 description numbers its first wrapper's serial engines from
zero.  Therefore SSC SPI instance 3 maps to mainline `spi3` at `0x00a8c000`
(QUPv3 SE3), and SSC I2C instance 2 maps to mainline `i2c2` at `0x00a88000`
(QUPv3 SE2).  The Caihong board currently enables `spi4` for the touchscreen;
`spi3` remains disabled, so this mapping does not overlap the touch controller.

`caihong-i2c2-sensors.dtsi` contains the registry-confirmed `0x30` MMC5603
and `0x39` TCS3701 child nodes.  It is an opt-in probe fragment: include it
only after confirming SSC has released I2C2 and the sensor rails are powered.
The official ICM42607 driver requires `vdd` and `vddio` regulators and is kept
out of DTS until those rails are mapped.  Its current upstream implementation
uses one-shot IIO reads and does not consume the registry's SSC IRQ number;
that IRQ remains a future buffered-sampling concern.

## Next sensor work

1. integrate and reboot-test `caihong-ssc.service` from the root filesystem;
2. test listener and sensor recovery across suspend/resume;
3. determine why the TCS3701 proximity SUID is unavailable while lux works;
4. expose the validated SSC streams to desktop consumers that require IIO or
   SensorProxy interfaces;
5. identify the exact `icm4x607` and MMC56x3x variants from firmware/runtime
   attributes without taking their buses from SSC;
6. identify the barometer only from evidence, not from a generic SM8650 parts
   list.

ST's `vendor/st/opensource` content in the OnePlus OSS branch is NFC/eSE
(`st21nfc`/`st54spi_gpio`), not this tablet's IMU stack, and remains intentionally
out of this sensor port.
