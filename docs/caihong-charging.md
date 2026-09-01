# OnePlus Pad Pro (caihong) charging bring-up

## Hardware confirmed from downstream DT

Caihong project `23926` uses two Southchip SC8547-family charge pumps at I2C
address `0x6f`, on separate QUP I2C hubs:

- hub 2: primary `SC8547A`, downstream compatible `oplus,sc8547a`
- hub 0: secondary `SC8547`/SC8547A-compatible device, downstream compatible
  `slave_vphy_sc8547`

The Oplus charging framework connects the two charge-pump ICs in parallel and
configures a 3000 mA input-current budget for each path. The primary chip also
contains Oplus VOOC PHY handling in the downstream driver; this standalone
mainline port deliberately separates the generic charge-pump hardware from the
proprietary VOOC protocol.

## Current standalone driver scope

`sc8547_cp.ko` currently performs a safe bring-up only:

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

The driver intentionally does **not** yet alter OVP/OCP/UCP thresholds, switch
charge-pump mode, enable the charge pump, configure the watchdog, or implement
VOOC PHY commands. Those writes must be added only after the real Caihong
register state is captured and the downstream initialization sequence is
translated.

The current source has been compiled successfully against Linux 6.12.96
headers with `W=1`. It still needs a compile/test against the exact Caihong
Linux 7.2 build tree before being considered target-kernel verified.

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

## First hardware test

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
both devices report plausible ADC values and capture registers `0x00..0x20`,
`0x2b..0x33`, `0x36`, and `0x3a` from the running downstream kernel if
possible.

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

Downstream Caihong uses `ocp_reg = <0x0b>` and `ovp_reg = <0x36>` for the
charge-pump configuration. These values are recorded here as reverse-engineering
evidence only; the standalone driver does not currently write them.

The downstream primary initialization also writes several additional registers
before fast charging (including protection limits and watchdog configuration).
Those writes are deliberately not copied wholesale: the next stage will decode
each field and keep VOOC-PHY-only registers separate from generic charge-pump
control.

## Next implementation steps

1. Translate primary and secondary reset/protection initialization separately.
2. Add watchdog control.
3. Add an explicitly gated manual 2:1/bypass and enable interface for lab
   testing.
4. Coordinate the primary and secondary CP paths.
5. Integrate the charge-pump pair with the normal USB-PD/PPS charging policy.
6. Treat VOOC/SuperVOOC PHY as a later, separate layer.
