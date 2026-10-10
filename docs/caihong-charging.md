# OnePlus Pad Pro (caihong) charging

Caihong uses two SC8547-family charge pumps at I2C address `0x6f` on separate
I2C buses. The hub-2 SC8547A is the primary pump and owns the VOOC PHY. The
hub-0 SC8547 is the secondary pump.

## Driver behavior

`sc8547_cp.ko` provides standard `power_supply` telemetry for both pumps and
implements two charging paths:

- SVOOC starts automatically after cable insertion. The primary exchanges the
  vendor protocol, applies the project-23926 battery temperature, state-of-charge
  and voltage curves, and enables the secondary once primary input current is
  high enough. The same session lowers current as the battery approaches its
  curve thresholds.
- USB PD PPS is coordinated by `qcom_battmgr`. `INPUT_VOLTAGE_LIMIT` supplies
  the negotiated source target and `CALIBRATE` provides prepare, continuous-run,
  cancel and restore operations. These properties are kernel coordination
  hooks; there is no raw register or manual pump sysfs interface.

Automatic VOOC recovery returns the USB path to normal, cycles the firmware
input-suspend vote, and re-arms DP/DM. This reproduces cable removal at the
adapter state-machine boundary without exposing a userspace reset control.

## Safety

The active paths retain the limits validated against the OnePlus project-23926
implementation:

- 10.5 V maximum SVOOC bus voltage;
- 3.5 A maximum per-pump input current and 6.5 A aggregate limit;
- 70 degrees C pump die limit;
- 0 to 45 degrees C battery temperature range;
- 4.48 V battery stop threshold;
- repeated hardware fault, insertion, switching, ADC and watchdog checks;
- role-correct VOUT sensing for the secondary pump, whose local VBAT ADC is not
  connected on this board.

Cancellation is observed by each monitoring cycle. Shutdown, driver removal,
cable removal and all error exits disable the pumps, stop their watchdogs,
restore DP/DM, remove the USB suspend vote and restore the board idle profile.

## Device tree

Only identity and physical role belong in the SC8547 child nodes:

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

The PMIC GLINK parent enables the board integration with
`oneplus,pps-read-buffer` and `oneplus,pps-auto-start`. Register profiles,
current requests, session duration and protection thresholds are driver-owned
constants rather than device-tree tuning properties.

## Runtime interface

The two power supplies are named from their bus and address, for example
`sc8547-1-6f` and `sc8547-0-6f`. They expose charge state, adapter/battery
presence, VBUS, VOUT, VBAT, IBUS and die temperature. Normal operation does not
require a userspace service or a write to an SC8547-specific sysfs group.

Kernel logs record session entry, curve changes, secondary-pump transitions,
safety failures and final cleanup. They intentionally omit raw register dumps
and manual development controls.
