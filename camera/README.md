# Caihong SM8650 camera port

This directory tracks camera bring-up for the OnePlus Pad Pro
(`oneplus,caihong`) on Linux 7.2.

## Architecture decision

Linux 7.2 already contains the SM8650 camera infrastructure we need:

- `qcom,sm8650-camss` support in the mainline Qualcomm CAMSS driver;
- SM8650 CSIPHY/VFE hardware data;
- `qcom,sm8650-cci` support in the mainline Qualcomm CCI I2C controller;
- an upstream SM8650 camera DT binding and working SM8650 camera-card examples.

Therefore this port does **not** import the Android Qualcomm Spectra/CRM/CPAS
camera stack into mainline.  That downstream stack remains a hardware and
behaviour reference only.  Importing it would duplicate native CAMSS/CCI,
preserve the CamX packet/request ABI and add unnecessary Android/GKI coupling.

The mainline plan is instead:

1. use the existing SM8650 CAMSS/CCI/CSIPHY drivers;
2. describe Caihong's camera topology with a normal media graph;
3. add only missing physical V4L2 devices (sensor, lens/actuator, calibration
   access where necessary);
4. use the existing PM8550 flash driver rather than porting the downstream flash
   layer.

## Downstream reference

Hardware information comes from:

- repository: `OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650`
- branch: `oneplus/sm8650_b_16.0.0_pad_pro`
- camera driver reference: `vendor/qcom/opensource/camera-kernel`
- common camera DT: `vendor/qcom/proprietary/camera-devicetree/pineapple-camera.dtsi`
- Caihong overlay: `vendor/qcom/proprietary/camera-devicetree/oplus/caihong-camera-overlay.dts`
  and `caihong_camera_overlay_common.dtsi`

## Caihong hardware confirmed from downstream DT

Caihong uses camera CCI0 for both physical cameras:

| Camera | CCI | mainline bus | CSIPHY | MCLK | Reset | Other confirmed hardware |
| --- | --- | --- | --- | --- | --- | --- |
| rear (`cell-index = 0`) | CCI0 master 1 | `cci0_i2c1` | CSIPHY1 | MCLK1, 19.2 MHz | GPIO82 | GT9772 actuator, rear EEPROM, PM8550 flash |
| front (`cell-index = 1`) | CCI0 master 0 | `cci0_i2c0` | CSIPHY4 | MCLK4, 19.2 MHz | GPIO7 | EEPROM/module name `sc820cs_caihong` |

The front module is identified as SmartSens SC820CS by the downstream Caihong
EEPROM/module naming. Public SC820CS implementations consistently identify the
sensor by registers `0x3107`/`0x3108`, chip ID `0xd154`. The downstream-style
8-bit I2C address `0x6c` maps to Linux 7-bit address `0x36`.

The rear image-sensor model is not named in the generic downstream
`qcom,cam-sensor` node and is deliberately not guessed. It must be identified
from camera metadata, EEPROM/probe information or hardware probing before a
rear V4L2 sensor driver is selected.

Downstream pineapple camera hardware relevant to comparison/debugging:

- CCI0: `0x0ac15000`, IRQ 426, 37.5 MHz source clock
- CSIPHY1: `0x0ace6000`, IRQ 478
- CSIPHY4: `0x0acec000`, IRQ 122

These resources already have mainline SM8650 counterparts.

## Current front-camera milestone

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
`cci0_i2c0 -> CAMSS CSIPHY4`. Some regulator phandle labels may require a simple
rename when this fragment is merged into the actual Caihong board DTS; the
underlying downstream rails are confirmed as 1.8 V I/O, 2.8 V analog and 1.2 V
core.

## Build and CI

The camera directory can be built separately from the rest of this repository:

```sh
make -C camera KDIR=/path/to/linux
```

A dedicated CI workflow builds it against arm64 Linux v7.2 with
`MEDIA_CONTROLLER` and `VIDEO_V4L2_SUBDEV_API` enabled. This is deliberately
separate from the repository-wide build because several older non-camera
bring-up drivers currently have their own v7.2 Werror/API cleanup pending.

## Next stages

1. Get the probe-only `sc820cs.ko` clean under the dedicated arm64 v7.2 CI.
2. Merge the front-camera DT graph into the real Caihong DTS and confirm:
   - CCI0 probe;
   - regulator/clock/reset sequencing;
   - SC820CS chip ID `0xd154`;
   - media graph registration.
3. Recover/validate Caihong SC820CS mode programming before enabling stream.
4. Add exposure, analogue gain, VBLANK and test-pattern V4L2 controls.
5. Identify and bring up the rear image sensor on `cci0_i2c1 -> CSIPHY1`.
6. Add GT9772 as a V4L2 lens subdevice if no suitable mainline driver exists.
7. Wire PM8550 flash and calibration/EEPROM handling using existing mainline
   facilities wherever possible.

The downstream Spectra tree is still valuable for power sequencing, topology,
register/resource comparison and userspace-behaviour archaeology, but it is no
longer the codebase being transplanted into Linux 7.2.
