# SC8547 Stage 4 manual single-pump control

This document defines the **development-only** Stage 4 interface for manually
starting one SC8547-family charge pump after Stage 3 initialization has already
been validated.

Stage 4 is not automatic fast charging. It is a laboratory control surface for
testing one pump at a time. Dual-pump coordination and USB-PD/PPS policy belong
to later stages.

## Prerequisites

Do not attempt Stage 4 until the same pump has passed the Stage 0-3 test gates:

1. stable I2C probe and plausible ADC telemetry;
2. runtime silicon variant identified;
3. raw register snapshots captured;
4. Stage 3 experimental init completes with exact protection-register
   readback;
5. `charge_enabled=0` and `switching=0` after Stage 3 init;
6. normal PMIC/pmic-glink charging remains functional.

## Second explicit opt-in

Stage 4 control requires **both** development flags:

```dts
southchip,allow-experimental-control;
southchip,allow-experimental-cp-enable;
```

The Stage 4 writable attributes must not appear when the second property is
absent.

This keeps a Stage-3 DTS, which can modify protection registers while leaving
the pump off, from accidentally gaining a pump-start control after a driver
update.

## Required voltage safety window

Stage 4 also requires four explicit test-window properties:

```dts
southchip,experimental-vbus-min-uv = <VBUS_MIN>;
southchip,experimental-vbus-max-uv = <VBUS_MAX>;
southchip,experimental-vbat-min-uv = <VBAT_MIN>;
southchip,experimental-vbat-max-uv = <VBAT_MAX>;
```

The driver deliberately provides no default values. All four must be present,
non-zero, and ordered `min < max`. An incomplete/invalid window prevents CP
enable.

These are **authorization bounds**, not charger-policy targets. They merely say
which already-observed input/battery voltages are acceptable for a manual lab
start. Stage 4 does not negotiate or change the USB source voltage.

Do not use a broad window as a substitute for understanding the test setup.

## Single-pump rule

Stage 4 has no knowledge of a peer pump yet. The operator must ensure the other
charge pump remains disabled while validating one device.

Recommended order:

1. primary only;
2. primary disable and verify clean stop;
3. secondary only;
4. secondary disable and verify clean stop;
5. only after both succeed, implement/test Stage 5 dual-pump coordination.

## Stage 4 sysfs controls

With all opt-ins present, the experimental group gains:

```text
.../<bus>-006f/sc8547_experimental/
```

Additional attributes:

- `enable_window` (read-only): configured VBUS/VBAT authorization bounds.
- `work_mode` (read/write): `2:1` or `bypass`.
- `cp_enable` (read/write): `0` or `1`.

`work_mode` and `cp_enable` are development-only controls, not an upstream ABI.

## Work-mode rules

Writing `work_mode`:

1. requires Stage 3 `init_state=initialized`;
2. accepts only `2:1` and `bypass`;
3. reads `REG07[7]` first;
4. refuses mode changes while CP is enabled;
5. changes only `REG09[7]` with a masked update;
6. reads the bit back and fails if it does not match.

No other `REG09` bit, including watchdog bits, is intentionally changed.

## CP-enable preflight

Writing `1` to `cp_enable` is accepted only when **all** checks pass:

1. Stage 4 opt-in is present;
2. silicon variant is explicitly supported for control (currently SC8547 or
   SC8547A only);
3. Stage 3 `init_state=initialized`;
4. voltage safety window is complete and valid;
5. adapter-present status is asserted;
6. battery-present status is asserted;
7. CP is currently disabled;
8. no blocking fault/status bit is active;
9. VBUS ADC is within configured `[min,max]`;
10. VBAT ADC is within configured `[min,max]`.

Blocking conditions include at least:

- thermal shutdown;
- VBUS-low/VBUS-high error;
- VOUT OVP;
- VBAT OVP;
- IBAT OCP;
- VBUS OVP;
- IBUS OCP;
- IBUS UCP-fall.

A failed preflight performs no CP-enable write.

## CP-enable sequence

After preflight succeeds:

1. set only `REG07[7]` using a masked update;
2. wait for the charge pump to settle;
3. read `REG06` and require the CP-switching status bit;
4. read fault state again;
5. if switching is absent or a blocking fault appeared, immediately clear
   `REG07[7]` and return an error;
6. on success report `cp_enable=1`.

The initial implementation uses the downstream driver's conservative 500 ms
post-enable observation interval as bring-up evidence. This is not necessarily
a final policy latency.

## CP disable

Writing `0` to `cp_enable` is intentionally less restrictive and should always
attempt to clear `REG07[7]` when Stage 4 controls exist.

After disable:

1. clear only `REG07[7]`;
2. wait briefly;
3. read back CP-enable and switching status;
4. report an error if the enable bit did not clear.

Shutdown also continues to use the Stage-3 fail-closed path.

## Watchdog note

Stage 4 does not silently choose a watchdog timeout. The Stage-3 watchdog value
remains under explicit operator control.

Do not enable a short watchdog unless the expected refresh behavior has been
verified. The downstream code suggests chip activity/readback may participate
in watchdog handling, but Stage 4 does not rely on an unverified assumption.

## Example DTS template

Values below are placeholders and must be chosen from a known test setup:

```dts
charger@6f {
    compatible = "southchip,sc8547a";
    reg = <0x6f>;
    southchip,role = "primary";

    southchip,allow-experimental-control;

    southchip,experimental-reg00 = <0xXX>;
    southchip,experimental-reg02 = <0xXX>;
    southchip,experimental-reg04 = <0xXX>;
    southchip,experimental-reg05 = <0xXX>;

    southchip,allow-experimental-cp-enable;
    southchip,experimental-vbus-min-uv = <VBUS_MIN>;
    southchip,experimental-vbus-max-uv = <VBUS_MAX>;
    southchip,experimental-vbat-min-uv = <VBAT_MIN>;
    southchip,experimental-vbat-max-uv = <VBAT_MAX>;
};
```

## Suggested first manual test

Before touching Stage 4 controls:

```sh
cd /sys/bus/i2c/devices/<bus>-006f
cat sc8547/variant
cat sc8547/register_dump
cat sc8547/protection_state
cat sc8547/faults
cat sc8547_experimental/profile_raw
cat sc8547_experimental/init_state
cat sc8547_experimental/enable_window
```

Then explicitly initialize:

```sh
echo 1 > sc8547_experimental/apply_init
cat sc8547_experimental/init_state
cat sc8547/charge_enabled
cat sc8547/switching
```

Only after Stage 3 still looks correct, choose mode while disabled:

```sh
echo '2:1' > sc8547_experimental/work_mode
cat sc8547_experimental/work_mode
```

Record the pre-enable state:

```sh
cat sc8547/vbus_uv
cat sc8547/vbat_uv
cat sc8547/faults
cat sc8547/status_regs
```

Start the one pump:

```sh
echo 1 > sc8547_experimental/cp_enable
cat sc8547_experimental/cp_enable
cat sc8547/charge_enabled
cat sc8547/switching
cat sc8547/vbus_uv
cat sc8547/ibus_ua
cat sc8547/vbat_uv
cat sc8547/vout_uv
cat sc8547/faults
```

Stop it explicitly:

```sh
echo 0 > sc8547_experimental/cp_enable
cat sc8547/charge_enabled
cat sc8547/switching
```

## Acceptance gate for one pump

A pump passes Stage 4 only when repeated controlled tests show:

- requested mode reads back correctly while disabled;
- preflight rejects deliberately invalid conditions/window values;
- enable bit sets only after a successful preflight;
- switching status asserts after enable;
- ADC data remain plausible;
- no blocking fault appears;
- disable reliably clears the enable bit;
- switching stops cleanly;
- ordinary PMIC charging still recovers/continues as expected.

Primary and secondary must pass separately before Stage 5.

## Merge rule

Do not cherry-pick Stage 4 control code into `main` until Stage 3 has passed on
real hardware and a controlled single-pump test environment is available.
Documentation may be merged earlier.
