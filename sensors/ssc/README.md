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
`caihong-ssc.service`.

## Runtime validation

On the OnePlus Pad Pro with the stock Caihong ADSP firmware, the listener has
been validated with `libssc 0.4.4` and `ssccli`:

- accelerometer: live three-axis samples;
- gyroscope: live three-axis samples;
- magnetometer and compass: live samples;
- ambient light: live lux samples;
- proximity: SUID unavailable in the current registry/runtime and still open.

The ADSP remains running while the listener serves and persists the registry.
The generated `DIR`, `parsed_file_list.csv`, and `sns_reg_version` files confirm
that the firmware completed its registry initialization.
