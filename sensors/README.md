# Caihong non-camera sensors

This directory tracks ordinary sensor bring-up separately from the Qualcomm
camera stack.

## Important downstream architecture distinction

The OnePlus `vendor/oplus/sensor` tree is mostly an Android/Qualcomm sensor
support layer (`sensor_devinfo`, tracing, virtual sensors and command plumbing),
not a collection of standalone accelerometer/gyro/ALS Linux I2C drivers.
On SM8650 many physical sensors are expected to be owned by the Qualcomm sensor
DSP/SSC path.

Blindly importing that Oplus layer into mainline would therefore not create
normal Linux IIO devices. The preferred Linux 7.2 strategy is:

1. identify every physical sensor IC and its bus/interrupt/power topology from
   Caihong DT, firmware metadata and hardware probing;
2. check whether Linux 7.2 already has a matching IIO/input driver;
3. add only missing chip support as an out-of-tree driver;
4. use standard IIO/input bindings and interfaces where practical;
5. treat SSC/remoteproc integration as a separate problem for sensors that are
   not directly accessible from the application processor.

ST's `vendor/st/opensource` content in this OnePlus source branch is NFC/eSE
(`st21nfc`/`st54spi_gpio`), not the tablet IMU stack, so it is intentionally not
copied here as a motion-sensor driver.

## Next evidence to collect

- exact accelerometer/gyroscope IC
- ALS/proximity IC
- magnetometer if fitted
- hall/lid sensors
- which devices are AP-attached versus SSC-owned
- bus addresses, IRQ GPIOs and regulator rails

No speculative sensor module is enabled from the root Makefile until its actual
Caihong hardware is confirmed.
