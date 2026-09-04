# Caihong SM8650 camera port

This directory tracks camera bring-up for the OnePlus Pad Pro
(`oneplus,caihong`, project 23926) on Linux 7.2.

## Architecture decision

Linux 7.2 already contains the SM8650 camera infrastructure we need:

- `qcom,sm8650-camss` support in the mainline Qualcomm CAMSS driver;
- SM8650 CSIPHY/VFE hardware data;
- `qcom,sm8650-cci` support in the mainline Qualcomm CCI I2C controller;
- an upstream SM8650 camera DT binding and working SM8650 camera-card examples.

Therefore this port does **not** import the Android Qualcomm Spectra/CRM/CPAS
camera stack into mainline. That downstream stack remains a hardware and
behaviour reference only. Importing it would duplicate native CAMSS/CCI,
preserve the CamX packet/request ABI and add unnecessary Android/GKI coupling.

The mainline plan is instead:

1. use the existing SM8650 CAMSS/CCI/CSIPHY drivers;
2. describe Caihong's camera topology with a normal media graph;
3. add only missing physical V4L2 devices (sensor, lens/actuator, calibration
   access where necessary);
4. use the existing PM8550 flash driver rather than porting the downstream flash
   layer.

## Downstream and device-image references

Hardware information comes from:

- OnePlus OSS repository:
  `OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650`
- branch: `oneplus/sm8650_b_16.0.0_pad_pro`
- camera driver reference: `vendor/qcom/opensource/camera-kernel`
- common camera DT: `vendor/qcom/proprietary/camera-devicetree/pineapple-camera.dtsi`
- Caihong overlay:
  `vendor/qcom/proprietary/camera-devicetree/oplus/caihong-camera-overlay.dts`
  and `caihong_camera_overlay_common.dtsi`
- Caihong vendor camera metadata, especially
  `/odm/etc/camera/CameraHWConfiguration.config` and the QTI sensor-module
  blobs under `/odm/lib64/camera/`.

## Caihong camera hardware

Caihong uses camera CCI0 for both physical cameras:

| Camera | Sensor | CCI | mainline bus | CSIPHY | MCLK | Reset | Other confirmed hardware |
| --- | --- | --- | --- | --- | --- | --- | --- |
| rear (`cell-index = 0`, camera id 0) | SmartSens **SC1320CS** | CCI0 master 1 | `cci0_i2c1` | CSIPHY1 | MCLK1, 19.2 MHz | GPIO82 | GT9772 actuator, rear EEPROM, PM8550 flash |
| front (`cell-index = 1`, camera id 1) | SmartSens **SC820CS** | CCI0 master 0 | `cci0_i2c0` | CSIPHY4 | MCLK4, 19.2 MHz | GPIO7 | front EEPROM |

The sensor models are no longer inferred only from EEPROM names. Caihong's own
`CameraHWConfiguration.config` explicitly lists:

```text
Name[0] = sc1320cs
Name[1] = sc820cs
```

and its camera-id tables identify camera id 0 as rear and camera id 1 as front.
The vendor image also contains matching QTI sensor-module/tuning blobs for both
SC1320CS and SC820CS.

The downstream camera rails are:

- L4B: 1.8 V camera I/O;
- L16B: 2.8 V camera analog;
- L2G: 1.2 V camera digital/core;
- L9B: 2.8 V rear autofocus/actuator rail.

Downstream pineapple camera hardware relevant to comparison/debugging:

- CCI0: `0x0ac15000`, IRQ 426, 37.5 MHz source clock;
- CSIPHY1: `0x0ace6000`, IRQ 478;
- CSIPHY4: `0x0acec000`, IRQ 122.

These resources already have mainline SM8650 counterparts.

## Front SC820CS milestone

`sc820cs.c` is intentionally a safe **probe-only** V4L2 driver. It currently:

- acquires DOVDD/AVDD/DVDD, MCLK and reset GPIO;
- uses the Caihong 19.2 MHz input clock;
- powers the sensor only long enough to read its ID;
- checks `0x3107/0x3108 == 0xd154`;
- exposes one 3264x2448 RAW10 source pad;
- validates a four-lane CSI-2 endpoint;
- registers a normal V4L2 sensor subdevice;
- explicitly returns `-EOPNOTSUPP` when userspace tries to start streaming.

No foreign-device register table is written during this stage. Public SC820CS
mode tables are useful references, but they are not assumed to be Caihong's
exact tuning/configuration.

`caihong-front-sc820cs.dtsi` maps the front sensor onto mainline
`cci0_i2c0 -> CAMSS CSIPHY4`. The downstream-style 8-bit SC820CS address `0x6c`
corresponds to Linux 7-bit address `0x36`; this remains an item to verify on the
actual Caihong bus together with the chip ID.

## Rear SC1320CS milestone

The rear sensor is now conclusively identified as SC1320CS. SmartSens documents
it as a 13 MP, 4224x3134, 30 fps MIPI sensor.

The remaining probe parameters are deliberately still marked unknown until they
are recovered from Caihong's QTI `com.qti.sensormodule.lce_sc1320cs.bin` or from
a read-only hardware probe:

- 7-bit CCI/I2C slave address;
- chip-ID register address/data width;
- expected chip-ID value/mask.

Do **not** copy an SC1320CS address or ID from an unrelated phone and call it a
Caihong value. The first rear driver should mirror the front strategy: power,
reset, read-only identification and media-subdevice registration before any
mode table is written.

## GT9772 autofocus milestone

`gt9772.c` implements the rear Giantec GT9772 as a V4L2 lens subdevice using
Linux's `v4l2-cci` helpers. Qualcomm's GT9772 actuator data confirms:

- downstream 8-bit slave address `0x18`, therefore Linux 7-bit `0x0c`;
- 10-bit focus DAC;
- focus register `0x03` with 16-bit data;
- initialization writes `ED=AB`, `06=84`, `07=01`, `08=55`;
- initial/park code 40;
- approximately 10 ms rail settle time and 100 us after each initialization
  register write.

`caihong-rear-gt9772.dtsi` places it on `cci0_i2c1` and uses the confirmed
camera I/O and AF rails (L4B 1.8 V, L9B 2.8 V).

## Build and CI

The camera directory can be built separately from the rest of this repository:

```sh
make -C camera KDIR=/path/to/linux
```

The dedicated camera CI uses a minimal arm64 Linux v7.2 configuration with the
required media-controller/V4L2/CCI, I2C, regulator, GPIO, clock and runtime-PM
frameworks built in. It runs `modules_prepare`, builds a real `vmlinux`, hands
the genuine v7.2 `vmlinux.symvers` export table to external modpost, and then
builds the out-of-tree camera modules with strict modpost.

The initial camera milestone is now CI-clean: run `33865161174` successfully
compiled and final-linked both modules through the complete external-module
pipeline:

```text
CC [M]  sc820cs.o
CC [M]  gt9772.o
MODPOST Module.symvers
CC [M]  sc820cs.mod.o
CC [M]  gt9772.mod.o
LD [M]  sc820cs.ko
LD [M]  gt9772.ko
```

This proves Linux-v7.2 arm64 source/API and exported-symbol compatibility. It
does **not** by itself prove the Caihong electrical mapping or successful probe
on hardware.

## Next stages

1. Merge the front-camera graph into the real Caihong DTS and confirm CCI0,
   clocks, rails, reset and SC820CS ID `0xd154` on hardware.
2. Recover SC1320CS probe address/ID from the Caihong sensor-module blob or a
   read-only bus probe, then add a safe rear probe-only V4L2 driver.
3. Recover/validate Caihong SC820CS and SC1320CS mode programming before
   enabling streaming.
4. Add exposure, analogue gain, VBLANK and test-pattern controls only after the
   correct mode tables are proven.
5. Wire PM8550 flash and calibration/EEPROM handling using existing mainline
   facilities wherever practical.
6. Bring up the complete media graph under libcamera before considering any
   downstream CamX compatibility layer.

The downstream Spectra tree is still valuable for power sequencing, topology,
register/resource comparison and userspace-behaviour archaeology, but it is no
longer the codebase being transplanted into Linux 7.2.
