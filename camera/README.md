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
| rear (`cell-index = 0`, camera id 0) | SmartSens **SC1320CS** | CCI0 master 1 | `cci0_i2c1`, **0x36** | CSIPHY1 | MCLK1, 19.2 MHz | GPIO82 | GT9772 actuator, rear EEPROM, PM8550 flash |
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

`sc820cs.c` is now a working first-stage V4L2 streaming driver. It currently:

- acquires DOVDD/AVDD/DVDD, MCLK and reset GPIO;
- uses the Caihong 19.2 MHz input clock;
- powers the sensor for identification and on-demand streaming;
- checks `0x3107/0x3108 == 0xd154`;
- exposes one 3264x2448 RAW10 source pad;
- validates a four-lane CSI-2 endpoint;
- reports the Caihong 366 MHz CSI link frequency through `get_mbus_config`;
- registers a normal V4L2 sensor subdevice;
- writes the official Caihong SC820CS initialization table before streaming;
- starts and stops the sensor through the normal V4L2 `s_stream` callback.

The initialization table is the 117-entry `sc820cs_setting` sequence from
Caihong's downstream camera tree, with its final `0x0100 = 0x01` entry removed
because the V4L2 driver performs stream-on explicitly. The board-specific
`0x301f = 0x0e` value and Caihong's power topology are retained; the Lenovo
Y700 address and power settings are not copied.

The tested B-slot image enables the upstream SM8650 `camcc`, CCI, CAMSS and
CSIPHY4 graph, then loads `sc820cs.ko` from initramfs. On hardware it logged
`SC820CS detected, chip ID 0xd154` and captured two consecutive
3264x2448 RAW10 frames (`pBAA`, 9,987,840 bytes each) through `/dev/video0`.
The validated image is
`mainline-boot-v2-stage6b-front-camera-stream-v2-linkfreq.img` with SHA-256
`5976133f831b10862350c32e845046612b47eef5ff442bfdaec28559264cd05b`.

CAMSS exposes several possible CSIPHY/CSID/VFE paths, so the non-immutable
downstream links must currently be enabled by media-controller userspace. The
validated path is:

```text
sc820cs 8-0010 -> msm_csiphy4 -> msm_csid0 -> msm_vfe0_rdi0 -> /dev/video0
```

The media formats on that path are `SBGGR10_1X10/3264x2448`; the video node
format is the packed `pBAA` fourcc. A camera service or libcamera pipeline
handler should perform this graph setup before opening the node.

`caihong-front-sc820cs.dtsi` maps the front sensor onto mainline
`cci0_i2c0 -> CAMSS CSIPHY4`. A powered read-only probe on Caihong found the
SC820CS at Linux 7-bit address `0x10` and returned chip ID `0xd154`. This differs
from the Lenovo Y700 reference driver, which uses `0x36`; that address must not
be copied to Caihong. On SM8650, CSIPHY4 uses the
shared `vdd-csiphy24-*` resource group; the board fragment now names that group
explicitly so CAMSS does not substitute dummy regulators for the active PHY.
The board DTS also selects GPIO7 as the SC820CS reset output, matching the
official `cam_sensor_active_rst2` state; without this pinctrl state GPIO7 keeps
the default `dmic1_data` function and the sensor can remain held in reset.

## Rear SC1320CS milestone

The rear sensor is now conclusively identified as SC1320CS. SmartSens documents
it as a 13 MP, 4224x3134, 30 fps MIPI sensor.

Rear identification and RAW streaming have now been completed on Caihong. The
driver powers the sensor through L4B/L16B/L2G, drives MCLK1 at 19.2 MHz,
releases GPIO82, and reads the two 8-bit chip-ID registers:

- Linux 7-bit CCI/I2C address: **0x36**;
- chip-ID registers: **0x3107/0x3108**;
- chip ID: **0xc658**;
- CCI path: **CCI0 master 1 (`cci0_i2c1`)**.

The driver exposes the official Caihong 4208x3120 RAW10 mode over four CSI-2
lanes. Its initialization table is the 143-entry `sc1320cs_setting` sequence
from the OnePlus tree, with the final `0x0100 = 0x01` entry removed so stream
start and stop remain under the V4L2 `s_stream` callback. The DT uses a 600 MHz
link frequency and maps the sensor to CSIPHY1.

The validated media path is:

```text
sc1320cs 9-0036 -> msm_csiphy1 -> msm_csid1 -> msm_vfe1_rdi0 -> /dev/video3
```

On hardware, a single frame and a subsequent three-frame run completed without
CSI, VFE, overflow or timeout errors. Each packed `pBAA` frame is 4208x3120
with a 5264-byte stride (16,423,680 bytes total), and all three frames had
different hashes. The first decoded frame contained a coherent real scene;
its 10-bit samples ranged from 63 to 203 with no zero or saturated pixels.
The tested B-slot image is
`mainline-boot-v2-stage6b-front-rear-camera-stream-v1.img`, SHA-256
`5300bbb60f0931164f09656bfb4a324a539e32ed121c73cbd31dc1bd718eec9f`.

Autofocus, EEPROM, flash, sensor exposure/gain controls and a camera userspace
pipeline are separate follow-up stages.

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

The actuator is now hardware-validated in the real Caihong DTS. It registered
at `9-000c` as `/dev/v4l-subdev30` with a `focus_absolute` range of 0..1023.
Control writes at 40, 256 and 800 all completed without CCI errors while the
rear sensor captured full frames. Comparing the same scene at 40 and 800
showed a clear optical focus change, confirming physical lens movement rather
than only successful bus writes. The final control value was restored to the
park code 40, and runtime suspend/resume exercised the driver's park and
restore paths during the test.

The validated autofocus image is
`mainline-boot-v2-stage6b-front-rear-camera-focus-v2.img`, SHA-256
`38a1b98a7d552fd73087bee7e9c4c692d8361b214739d5d75d42494cf5c6869e`.

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

1. Add exposure, analogue gain, VBLANK and test-pattern controls to both
   SmartSens sensor drivers.
2. Make the media-graph setup automatic through the camera userspace stack and
   validate the path with libcamera.
3. Wire PM8550 flash and calibration/EEPROM handling using existing mainline
   facilities wherever practical.
4. Bring up the complete media graph under libcamera before considering any
   downstream CamX compatibility layer.

The downstream Spectra tree is still valuable for power sequencing, topology,
register/resource comparison and userspace-behaviour archaeology, but it is no
longer the codebase being transplanted into Linux 7.2.
