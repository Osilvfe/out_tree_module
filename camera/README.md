# Caihong SM8650 camera port

This directory tracks the Linux 7.2 out-of-tree port of the Qualcomm/OnePlus
camera stack used by the OnePlus Pad Pro (`oneplus,caihong`).

## Downstream source

The reference code comes from:

- repository: `OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650`
- branch: `oneplus/sm8650_b_16.0.0_pad_pro`
- camera driver: `vendor/qcom/opensource/camera-kernel`
- common camera DT: `vendor/qcom/proprietary/camera-devicetree/pineapple-camera.dtsi`
- Caihong overlay: `vendor/qcom/proprietary/camera-devicetree/oplus/caihong-camera-overlay.dts`
  and `caihong_camera_overlay_common.dtsi`

The downstream `pineapple.mk` enables the whole Spectra stack. The first
mainline-7.2 bring-up will deliberately limit the import to the camera base plus
`CONFIG_SPECTRA_SENSOR`; ISP/ICP/JPEG/CRE are deferred until the control plane,
CCI and CSIPHY compile and probe cleanly.

## Why SENSOR is not a standalone module

Downstream links all camera objects into one `camera.ko`. The base object set
contains Camera Request Manager, memory manager, common utilities, SMMU, sync,
CPAS and CDM. The `CONFIG_SPECTRA_SENSOR` object set then adds CCI, CSIPHY,
sensor, actuator, EEPROM, OIS, TPG, flash/resource management and the camera
I2C/I3C/SPI access layer.

This dependency is real rather than a Kbuild accident. For example, CCI starts
CPAS before enabling its platform resources, CSIPHY queries CPAS hardware state,
and the sensor core uses the downstream Camera Request Manager packet/request
ABI. Therefore the first compatibility target is a reduced `camera.ko` made
from BASE + SENSOR, not an artificial standalone `cam_sensor.ko`.

## Caihong hardware confirmed from downstream DT

Caihong uses camera CCI0 for both physical cameras:

| Camera | CCI | master | CSIPHY | MCLK | Reset | Other confirmed hardware |
| --- | --- | --- | --- | --- | --- | --- |
| rear (`cell-index = 0`) | CCI0 | master 1 | CSIPHY1 | MCLK1, 19.2 MHz | GPIO82 | GT9772 actuator, rear EEPROM, PM8550 flash |
| front (`cell-index = 1`) | CCI0 | master 0 | CSIPHY4 | MCLK4, 19.2 MHz | GPIO7 | EEPROM name `sc820cs_caihong` |

The front EEPROM naming is strong evidence for a SmartSens SC820CS front
module. The rear image-sensor model is not named in the generic downstream
`qcom,cam-sensor` DT node and must not be guessed; it will be identified from
userspace camera metadata, EEPROM/probe tables or hardware probing before a
native V4L2 sensor driver is selected.

Downstream pineapple camera hardware relevant to this port includes:

- CCI0: `0x0ac15000`, IRQ 426, 37.5 MHz source clock
- CSIPHY1: `0x0ace6000`, IRQ 478
- CSIPHY4: `0x0acec000`, IRQ 122
- CSIPHY compatible: `qcom,csiphy-v2.2.0`

## Port stages

1. Import the exact downstream BASE + SENSOR source/header dependency closure
   without enabling it from the repository root Makefile.
2. Remove Android/GKI build assumptions and add a conventional external-module
   Kbuild entry.
3. Compile against the exact Linux 7.2 Caihong tree and patch kernel API drift.
4. Make CRM/base, CPAS, CCI and CSIPHY probe without touching image streaming.
5. Bring up sensor/EEPROM/actuator power and register access.
6. Identify the rear sensor and decide per sensor whether to retain the CamX
   packet ABI or use a native mainline V4L2 sensor driver.
7. Only then add ISP/CSID/IFE streaming, followed later by ICP/JPEG/CRE if they
   are required by the chosen userspace stack.

## Known Linux-7.2 compatibility work

The downstream code was written for the Qualcomm Android/GKI kernel and already
contains version conditionals that stop at older kernels. Expected first-pass
work includes:

- modern I2C driver probe/remove callback signatures;
- component-framework and V4L2 subdev API drift;
- dma-buf/dma-fence and IOMMU API changes in the camera base;
- downstream camera SMMU assumptions versus current DMA/IOMMU interfaces;
- CPAS clock/interconnect/RPMh integration;
- downstream-only Qualcomm/Android headers and generated build headers;
- removal or isolation of Oplus extensions that are not required by Caihong;
- DT binding cleanup after probe equivalence is established.

Do not add `camera.ko` to the root `Makefile` until the imported tree reaches a
useful compile milestone. This keeps the already-working touchscreen, pogo and
charging modules buildable while the camera port is in progress.
