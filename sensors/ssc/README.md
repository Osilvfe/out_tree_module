# Caihong Qualcomm SSC runtime

Caihong's accelerometer, gyroscope, magnetometer and ambient-light sensor are
owned by the Qualcomm Snapdragon Sensor Core (SSC) in the stock firmware. They
work on mainline Linux without moving their buses to the application processor
when the ADSP sensors process has a Hexagon FastRPC default listener.

The runtime uses the upstream
[`linux-msm/hexagonrpc`](https://github.com/linux-msm/hexagonrpc) implementation.
It is pinned to PR 27 commit `40e50412812a`, which adds the writable registry
path required by this firmware. Refusing the first write to `registry/DIR`
causes the stock ADSP firmware to assert in `sns_registry_sensor.c`.

The local patch adds two Caihong requirements:

- expose the stock `/odm/etc/sensor/config` tree to the DSP;
- truncate registry files when an ODM entry replaces a longer vendor entry.

## Build a redistributable layout

The proprietary registry JSON and DSP libraries remain in the user's extracted
stock firmware and are not committed here. Generate a bundle from `vendor.img`
and `odm.img`:

```sh
EROFS_FSCK=/path/to/fsck.erofs \
  ./build-bundle.sh /path/to/extracted-firmware /path/to/output
```

The script builds native `sscregistrygen`, cross-builds `hexagonrpcd`, applies
the vendor configuration followed by the Caihong ODM overrides, and creates a
writable registry seed. It selects the stock filters `MTP` and SoC ID `577`.

Install the resulting bundle into an offline root filesystem:

```sh
sudo ./install-rootfs.sh /path/to/output/bundle /mounted/rootfs
```

This installs the program under `/usr/local`, keeps mutable registry and
calibration data under `/var/lib/caihong-ssc`, and enables
`caihong-ssc.service`. It also enables Caihong's SSC accelerometer, light and
compass backends in `iio-sensor-proxy`, ordered after the FastRPC listener.
The udev rule rotates the SSC coordinates into the panel orientation with the
Caihong mount matrix and marks the accelerometer as display-mounted.

## Runtime validation

On the OnePlus Pad Pro with the stock Caihong ADSP firmware, the listener has
been validated with `libssc 0.4.4` and `ssccli`:

- accelerometer: live three-axis samples;
- gyroscope: live three-axis samples;
- magnetometer and compass: live samples;
- ambient light: live lux samples;
- RGB/CCT: the vendor `rgb` SUID publishes the calibrated 16-float TCS3701
  payload at about 10 Hz; its lux field matches `ambient_light` samples;
- proximity: intentionally absent from the stock Caihong sensor declaration,
  so the SSC firmware does not publish a proximity SUID.

The ADSP remains running while the listener serves and persists the registry.
The generated `DIR`, `parsed_file_list.csv`, and `sns_reg_version` files confirm
that the firmware completed its registry initialization. The systemd service
and live sensor streams recover automatically after cold boot and deep
suspend/resume. `iio-sensor-proxy 3.9` exposes accelerometer orientation,
ambient lux and compass heading through its standard D-Bus API; `monitor-sensor
--all` receives live updates from all three backends.

KDE defaults internal-panel rotation to `inTabletMode`, but Caihong has no
`SW_TABLET_MODE` input switch. Enable automatic rotation for the current user
once the SSC service is running:

```sh
kscreen-doctor output.DSI-1.autoRotatePolicy.always
```

KWin persists this policy in the user's `kwinoutputconfig.json`. The four
physical orientations and automatic landscape/portrait changes are validated.

## Diagnostic tool

`tools/ssc-inspect` queries arbitrary SSC data types and can subscribe to the
standard float-array event format without taking ownership of the physical
I2C or SPI buses:

```sh
cd tools/ssc-inspect
make
./ssc-inspect rgb ambient_light
./ssc-inspect --stream rgb 5
```

The tool is diagnostic only and is not installed into the root filesystem by
`install-rootfs.sh`.
