# SC8547 development branch roadmap

This document tracks the forward-development branch for the OnePlus Pad Pro
(`caihong`) dual-SC8547 charge-pump port.

The stable/testable branch is `main`. Ongoing work lives on `sc8547-next`.
Changes should move from `sc8547-next` to `main` only after the corresponding
hardware test gate below has passed. Prefer cherry-picking individual commits
instead of merging the whole development branch.

## Branch policy

- `main`: conservative code intended for actual device testing.
- `sc8547-next`: may contain unverified control paths and incomplete policy
  integration.
- Every functional stage should be a separate commit or small commit series.
- A stage that writes charge-pump control/protection registers must not be
  silently enabled by merely adding the normal device node.
- VOOC/SuperVOOC/UFCS protocol support is intentionally separate from generic
  charge-pump control.

## Hardware topology

Caihong downstream DT uses two SC8547-family devices at I2C address `0x6f` on
separate buses:

- QUP I2C hub 2: primary, downstream `oplus,sc8547a`
- QUP I2C hub 0: secondary, downstream `slave_vphy_sc8547`

Both downstream nodes carry:

```dts
ocp_reg = <0xb>;
ovp_reg = <0x36>;
```

The downstream virtual charge-pump layer connects the two devices in parallel
and assigns a 3000 mA input-current budget to each path.

## Important silicon compatibility rule

SC8547 and SC8547A share the core charge-pump and ADC data map used by this
port. In particular, VBUS/IBUS/VBAT/VOUT/VAC/TDIE data locations and scales are
shared in the Oplus source.

They are **not** register-for-register identical for control programming.
Known differences include the `REG05` IBUS-UCP deglitch field and downstream
`REG07` low-bit setup. Therefore common control code must use masked updates for
shared bits such as CP enable and work mode, and variant-specific helpers for
fields that differ.

SC8547A additionally exposes UFCS registers starting at `0x40`; those are not
part of the generic charge-pump bring-up.

## Protection-value caution

Do not infer electrical thresholds from downstream comments alone.

The common Oplus SC8547 header describes BAT_OVP (`REG00[5:0]`) as:

- base: 3500 mV
- step: 25 mV

Under that definition code `0x36` would decode to 4850 mV. Some downstream
source comments near the same configuration state 4.65 V instead. Because the
source comments, encoded project value, and header formula are not fully
self-consistent, the development driver must keep raw values visible and must
not automatically program this protection profile until hardware behavior is
verified.

The same rule applies to any other threshold where the vendor comments and
register definitions disagree.

## Stage 0 - baseline telemetry

Status: implemented on `main`.

Writes performed automatically:

- ADC enable only (`REG11[7]`)

Features:

- I2C/regmap probe
- device ID
- ADC telemetry
- CP enable/mode/switching state
- status/fault decoding
- `power_supply` read-only reporting

### Test gate

For both `0x6f` devices verify:

1. probe succeeds without I2C errors;
2. primary ID is plausibly SC8547A (`0x67` expected from downstream);
3. voltage/current/temperature data are plausible;
4. `charge_enabled` remains off unless firmware/downstream left it on;
5. status/fault bits are plausible with charger unplugged and plugged.

Only after this gate should Stage 0 be considered device-verified.

## Stage 1 - variant model and register snapshot

Status: implemented on `sc8547-next` in commit `60c230c`.

New behavior:

- explicit SC8547/SC8547A/SC8547D/unknown variant model;
- DT-compatible versus runtime-ID mismatch warning;
- read-only `variant` attribute;
- read-only common-register dump;
- internal masked helpers for ADC/CP/mode control.

No new automatic CP/protection write is introduced.

### Test gate

Read for both devices:

```sh
cat /sys/bus/i2c/devices/<bus>-006f/sc8547/variant
cat /sys/bus/i2c/devices/<bus>-006f/sc8547/register_dump
```

Save both dumps with charger unplugged and with a normal 5 V source attached.
This provides the reference state for later protection/control stages.

## Stage 2 - protection model, decode only

Planned next.

Goals:

- define the generic SC8547 protection register fields;
- decode current raw protection settings to human-readable values;
- preserve raw register values beside decoded values;
- model SC8547 versus SC8547A field differences;
- do **not** apply a new protection profile automatically.

This stage is intentionally read-only so it can be tested before enabling any
high-power path.

### Test gate

Compare decoded values against:

- raw register dump;
- downstream kernel register dump if available;
- observed normal charging state.

Any decode with contradictory vendor definitions remains labelled ambiguous
instead of being treated as authoritative.

## Stage 3 - controlled hardware init

Planned after Stage 2.

Goals:

- implement reset and generic CP initialization separately for primary and
  secondary;
- preserve VOOC/DPDM/UFCS registers;
- keep CP disabled after init;
- add watchdog programming helpers;
- require an explicit development-only DT opt-in before applying an unverified
  board profile.

### Test gate

After init and before CP enable:

1. CP remains disabled;
2. ADC remains functional;
3. no unexpected fault bits are asserted;
4. raw protection registers match the intended profile;
5. basic PMIC/pmic-glink charging still works.

## Stage 4 - manual single-pump control

Planned after Stage 3.

Goals:

- development-only manual 2:1/bypass selection;
- development-only CP enable/disable;
- masked writes only for common enable/mode bits;
- refuse enable when protection initialization has not completed;
- refuse enable for unknown silicon variant;
- optionally require VBUS/VBAT sanity checks before enable;
- verify `REG06` switching state after enable and fail closed if switching does
  not start.

No automatic fast-charge policy at this stage.

### Test order

1. secondary disabled, primary only;
2. verify 2:1 mode at a controlled input voltage/current;
3. disable primary and verify clean stop;
4. repeat on secondary independently;
5. only then proceed to dual-pump coordination.

## Stage 5 - dual-pump coordinator

Planned after both individual pumps pass Stage 4.

Goals:

- primary/secondary relationship;
- ordered enable/disable;
- aggregate telemetry;
- per-pump fault handling;
- fail closed to one pump or no pump as appropriate;
- no VOOC dependency.

The downstream project models the two pumps as parallel paths with a nominal
3000 mA input-current budget per path. That value is evidence, not permission
to immediately request 6 A from an arbitrary power source.

## Stage 6 - USB-PD/PPS policy integration

Planned after dual-pump operation is stable.

Goals:

- connect CP control to ordinary USB-PD/PPS negotiation;
- request source voltage/current before starting a direct-charge path;
- coordinate battery voltage, requested VBUS and CP ratio;
- ramp current rather than immediately requesting the final level;
- continuously handle thermal/fault/input-collapse conditions;
- fall back to normal PMIC charging on failure.

This stage is where the driver becomes useful for non-proprietary high-power
charging rather than only laboratory CP control.

## Stage 7 - proprietary protocol work

Optional/later.

Primary SC8547A contains VOOC/DPDM and UFCS-related functionality in the Oplus
source. These protocol blocks are deliberately not required for the generic
charge-pump driver. If ported, they should be separate layers with separate
review/testing.

## Suggested cherry-pick sequence

When hardware testing becomes available, move changes to `main` in this order:

1. Stage 1 variant/snapshot commit;
2. Stage 2 decode-only commit(s);
3. Stage 3 init/watchdog commit(s) only after protection values are confirmed;
4. Stage 4 manual-control commit(s) after safe lab tests;
5. Stage 5 dual-pump coordinator;
6. Stage 6 PD/PPS integration.

Never cherry-pick a later control stage merely because it compiles; each stage
assumes the previous hardware gate has passed.
