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

For mainline Linux the preferred architecture is therefore:

1. recover the actual SSC registry hardware configuration;
2. map SSC bus instances/power rails/pins back to AP-visible resources;
3. reuse or adapt upstream Linux IIO/input drivers wherever possible;
4. use standard Linux interfaces rather than importing the Oplus private
   sensor framework;
5. keep SSC/remoteproc coexistence and bus ownership as a separate bring-up
   problem.

## Hardware recovered from Caihong vendor sensor registry

The public Caihong vendor image contains
`/odm/etc/sensor/config/json_list`.  Its hardware entries identify the active
sensor set below.

| Function | Registry hardware | Downstream bus / IRQ | Mainline plan |
| --- | --- | --- | --- |
| accelerometer + gyroscope | `icm4x607` | `bus_type=1` (SPI), instance 3, IRQ 80, high-level, keeper; orientation `-x -y +z` | identify exact ICM42607-family variant, then adapt the upstream IIO support that landed after Linux 7.2; transport split is intentionally deferred |
| magnetometer | `mmc56x3x` | `bus_type=0` (I2C), instance 2, address 48 decimal (`0x30`), 100-400 kHz; orientation `+y -x +z` | `mmc5633.ko` is an external I2C-only variant derived from the MMC5603/MMC5633 driver already present in Linux v7.2; exact silicon compatible still needs runtime/firmware evidence |
| ALS / CCT | `tcs3701` through `sns_alsps` | I2C instance 2, address 57 decimal (`0x39`), IRQ 84 falling-edge, two sensor rails | `tcs3701.ko` provides a direct-I2C IIO bring-up path for raw clear/R/G/B and proximity data; IRQ thresholds, board calibration and lux/CCT conversion remain future work |
| Hall / lid | `bu52053nvx` | SoC TLMM GPIO66, dual-edge, no pull, one `sensor_vddio` rail | `bu52053nvx.ko` in this tree exposes standard `EV_SW/SW_LID` |
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

`sensors/bu52053nvx.c` is intentionally small and non-Oplus-specific:

- reads a GPIO only; there is no register bus for this Hall switch;
- reports `EV_SW/SW_LID` through the Linux input subsystem;
- handles both rising and falling edges as required by the Caihong SSC
  registry;
- supports wakeup;
- supports an optional `vddio-supply` regulator.

`sensors/caihong-bu52053nvx-hall.dtsi` maps the confirmed TLMM GPIO66 and uses
`GPIO_ACTIVE_LOW`, matching the BU52053NVX output behavior.  The downstream
registry names its rail only as `/pmic/client/sensor_vddio`, so the DTS fragment
leaves `vddio-supply` unset until the corresponding mainline PMIC regulator
phandle is proven.  With the property absent the driver assumes the board or
firmware keeps that rail powered.

## Next sensor work

1. map SSC SPI instance 3 and I2C instance 2 to the exact SM8650 QUP serial
   engines and pin states;
2. identify the exact `icm4x607` silicon variant from SSC firmware/WHO_AM_I;
3. identify MMC5603 vs MMC5633 from runtime/firmware evidence and select the
   matching DT compatible for `mmc5633.ko`;
4. hardware-test TCS3701 raw ALS/proximity over the AP-owned I2C path, then add
   IRQ thresholds and calibration only after the electrical path is proven;
5. recover the PMIC regulator behind `sensor_vddio` / `sensor_vdd`;
6. identify the barometer only from evidence (SSC registry/runtime info), not
   from a generic SM8650 parts list.

ST's `vendor/st/opensource` content in the OnePlus OSS branch is NFC/eSE
(`st21nfc`/`st54spi_gpio`), not this tablet's IMU stack, and remains intentionally
out of this sensor port.
