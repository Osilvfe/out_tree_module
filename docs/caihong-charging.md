# OnePlus Pad Pro (caihong) charging bring-up

## Development branches

- `main`: conservative/testable baseline. Keep this branch suitable for actual
  hardware bring-up.
- `sc8547-next`: forward-development branch. New control paths land here first
  and are intended to be cherry-picked to `main` stage by stage after testing.

The detailed development/test/merge gates are maintained in
[`docs/sc8547-next-roadmap.md`](sc8547-next-roadmap.md). Do not merge later
charge-pump control stages merely because they compile; each stage has a
hardware validation gate.

## Hardware confirmed from downstream DT

Caihong project `23926` uses two Southchip SC8547-family charge pumps at I2C
address `0x6f`, on separate QUP I2C hubs:

- hub 2: primary `SC8547A`, downstream compatible `oplus,sc8547a`
- hub 0: secondary `SC8547`/SC8547A-compatible device, downstream compatible
  `slave_vphy_sc8547`

Both downstream SC8547 nodes specify:

```dts
ocp_reg = <0xb>;
ovp_reg = <0x36>;
```

The Oplus charging framework connects the two charge-pump ICs in parallel and
configures a 3000 mA input-current budget for each path. The primary chip also
contains Oplus VOOC PHY handling in the downstream driver; this standalone
mainline port deliberately separates the generic charge-pump hardware from the
proprietary VOOC protocol.

## Current `main` scope

`sc8547_cp.ko` on `main` performs a safe bring-up only:

- 8-bit I2C regmap
- device-ID read from register `0x36`
- SC8547A ID `0x67` recognition
- SC8547D-compatible ID `0x49` recognition
- ADC enable only; the charge pump is not started automatically
- VBUS, IBUS, VBAT, VOUT, VAC and die-temperature telemetry
- existing charge-pump enable, switching state and 2:1/bypass mode reporting
- adapter-present and battery-present reporting
- decoded thermal/VBUS/OVP/OCP/UCP status
- `power_supply` registration for basic telemetry
- read-only bring-up attributes below the I2C device's `sc8547/` sysfs group

The driver intentionally does **not** alter OVP/OCP/UCP thresholds, switch
charge-pump mode, enable the charge pump, configure the watchdog, or implement
VOOC PHY commands.

The baseline source has been compiled successfully against Linux 6.12.96
headers with `W=1`. It still needs a compile/test against the exact Caihong
Linux 7.2 build tree before being considered target-kernel verified.

## `sc8547-next` additions

The development branch currently adds, without introducing new automatic
charge-pump/protection writes:

- explicit SC8547/SC8547A/SC8547D/unknown silicon variant model;
- warning when DT compatible and runtime device ID disagree;
- read-only `variant` attribute;
- read-only `register_dump` of the common charge-pump/ADC register space;
- masked internal helpers that preserve variant-specific low bits when later
  control stages use CP enable/mode/ADC bits.

These changes are the Stage 1 commit in the roadmap.

## Bring-up DTS

The following nodes are suitable for the telemetry-only stage. The
`southchip,role` property is local to this out-of-tree bring-up driver and is
only used for identification; it does not configure electrical behavior.

```dts
&i2c_hub_0 {
    clock-frequency = <400000>;
    status = "okay";

    charger@6f {
        compatible = "southchip,sc8547";
        reg = <0x6f>;
        southchip,role = "secondary";
    };
};

&i2c_hub_2 {
    clock-frequency = <400000>;
    status = "okay";

    charger@6f {
        compatible = "southchip,sc8547a";
        reg = <0x6f>;
        southchip,role = "primary";
    };
};
```

For comparison, the downstream aliases are also accepted by the driver:

- `oplus,sc8547a`
- `slave_vphy_sc8547`

Use the `southchip,*` compatibles in the mainline DTS while doing this port.

## First hardware test (`main`)

Build and install `sc8547_cp.ko`, boot with the two I2C nodes enabled, then
collect:

```sh
dmesg | grep -i sc8547
find /sys/bus/i2c/devices -path '*/sc8547/*' -type f -print
```

For each detected device, read the bring-up group (replace the I2C device path):

```sh
cd /sys/bus/i2c/devices/<bus>-006f/sc8547
cat device_id role
cat charge_enabled switching charge_mode
cat adapter_present battery_present
cat status_regs faults
cat vbus_uv ibus_ua vbat_uv vout_uv vac_uv tdie_mc
```

The ADC attributes use base units in their names: microvolts, microamps and
millicelsius.

Expected primary ID from the downstream SC8547A definition is `0x67`. Do not
enable high-voltage charging based only on successful probe; first verify that
both devices report plausible ADC values.

## Stage 1 test (`sc8547-next`)

In addition to the baseline test, capture:

```sh
cd /sys/bus/i2c/devices/<bus>-006f/sc8547
cat variant
cat register_dump
```

Capture `register_dump` for both pumps with the charger unplugged and again with
a normal 5 V source attached. Those snapshots are the reference for later
protection/init work.

## Downstream register facts already verified

Relevant registers used by the Oplus SC8547/SC8547A drivers:

- `0x06`: thermal state, VBUS error state and CP switching state
- `0x07[7]`: charge-pump enable
- `0x09[7]`: charge mode (`0` = 2:1, `1` = bypass/1:1)
- `0x0e`: OVP/OCP/UCP plus adapter/battery-present status
- `0x11[7]`: ADC enable
- `0x13..0x14`: IBUS ADC, 1.875 mA/LSB
- `0x15..0x16`: VBUS ADC, 3.75 mV/LSB
- `0x17..0x18`: VAC ADC, 5 mV/LSB
- `0x19..0x1a`: VOUT ADC, 1.25 mV/LSB
- `0x1b..0x1c`: VBAT ADC, 1.25 mV/LSB
- `0x1f..0x20`: die-temperature ADC, 0.5 C/LSB
- `0x36`: device ID

### SC8547 versus SC8547A

The core ADC/data map above is shared in the Oplus source, so one common
telemetry implementation is appropriate. Control programming is not completely
identical. One confirmed difference is `REG05` IBUS-UCP fall-deglitch encoding:
SC8547 uses a one-bit field while SC8547A exposes a two-bit field with extra
50/100 ms choices. The downstream secondary driver also preserves different
`REG07` low-bit defaults depending on whether runtime detection identifies an
SC8547A.

For that reason future control code uses masked updates for shared bits rather
than copying whole downstream register bytes unnecessarily.

### Protection-value ambiguity

Caihong uses raw project value `ovp_reg = <0x36>` on both pumps. The Oplus
common header defines BAT_OVP (`REG00[5:0]`) as 3500 mV + code * 25 mV, which
would decode raw code `0x36` as 4850 mV. However, comments in parts of the
vendor initialization source describe the configured BAT_OVP as 4.65 V.

This inconsistency is intentionally documented rather than silently resolved.
Until register behavior is confirmed on hardware, the development driver must
show raw protection values and decoded values but must not automatically apply
an assumed interpretation.

## Development order

See `docs/sc8547-next-roadmap.md` for the authoritative sequence. In short:

1. variant model and register snapshots;
2. protection decode only;
3. controlled reset/init and watchdog, CP still off;
4. explicitly gated single-pump manual control;
5. dual-pump coordination;
6. normal USB-PD/PPS policy integration;
7. optional VOOC/SuperVOOC/UFCS layers later.
