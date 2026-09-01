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
- Documentation is written before or together with each write-capable stage so
  the test and merge contract does not depend on unwritten context.

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
`REG07` low-bit setup. Therefore common control code uses masked updates for
shared bits such as CP enable and work mode, and variant-specific helpers for
fields that differ.

SC8547A additionally exposes UFCS registers starting at `0x40`; those are not
part of the generic charge-pump bring-up.

SC8547D is currently recognized for telemetry but deliberately rejected by the
experimental write path until its control compatibility is established.

## Protection-value caution

Do not infer electrical thresholds from downstream comments alone.

The common Oplus SC8547 header describes BAT_OVP (`REG00[5:0]`) as:

- base: 3500 mV
- step: 25 mV

Under that definition code `0x36` would decode to 4850 mV. Some downstream
source comments near the same configuration state 4.65 V instead.

A second example exists for IBUS OCP: the common header describes the low
nibble of `REG05` as 1200 mA + code * 300 mA. Project code `0x0b` therefore
decodes to 4500 mA by that header formula, while some nearby downstream source
comments describe 3.6 A. These discrepancies are preserved as evidence rather
than silently resolved.

Because the source comments, encoded project values, and header formulas are
not fully self-consistent, the development driver keeps raw values visible and
does not automatically derive a protection profile from the project values.

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

Status: implemented on `sc8547-next`.

Primary source commit:

- `60c230c` - variant model, runtime-ID warning, masked helpers and read-only
  register snapshot.

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

Status: implemented on `sc8547-next`.

Source commits:

- `bc553aa` - read-only protection decode;
- `f15fb5a` - explicit bitfield-helper include so the decode commit does not
  rely on indirect includes.

New read-only attribute:

```sh
cat /sys/bus/i2c/devices/<bus>-006f/sc8547/protection_state
```

It reports:

- raw `REG00/01/02/04/05/08/09/0d` values;
- BAT OVP code and vendor-header formula result;
- BAT OCP code and vendor-header formula result;
- AC/VBUS OVP codes and vendor-header formula results;
- IBUS UCP/OCP enable state;
- IBUS OCP code and vendor-header formula result;
- variant-aware IBUS-UCP deglitch time for known SC8547/SC8547A layouts;
- soft-start timeout code;
- PMID2OUT UVP/OVP codes;
- watchdog timeout code.

Every calculated threshold is labelled `header_*` or `header_formula_*` in the
sysfs output. This is deliberate: it means "decoded using the Oplus header",
not "electrically confirmed on this board".

No new automatic write is introduced in Stage 2.

### Test gate

Compare `protection_state` against:

- the same device's `register_dump`;
- downstream kernel register dump if available;
- unplugged versus normal-5-V-source state.

Specifically record whether the device actually contains project values related
to `0x36` and `0x0b`, and whether primary/secondary differ after boot.

Any decode with contradictory vendor definitions remains labelled ambiguous
instead of being treated as authoritative.

## Stage 3 - gated controlled hardware init

Status: implemented on `sc8547-next`, **not hardware-validated**.

Source commits in development history:

- `aa3c510` - initial gated experimental reset/init/watchdog controls;
- `29e6c6c` - role-safe profile handling, optional primary-only fields,
  telemetry-only shutdown safety, SC8547D/unknown write rejection;
- `17d7037a` - Linux v7.2 `power_supply` API and strict-format compatibility.

Stage-3 interface documentation:

- `docs/sc8547-experimental-control.md`
- latest alignment commit in the current history: `d6509c6`

The experimental controls exist only with:

```dts
southchip,allow-experimental-control;
```

Without that property the device remains telemetry-only apart from ADC enable.

Stage 3 requires explicit raw values for `REG00/02/04/05`; `REG01` and `REG0d`
are optional because primary and secondary vendor init sequences are not
identical. It does not synthesize a profile from downstream `ovp_reg`/`ocp_reg`
project values.

`apply_init`:

1. accepts only SC8547/SC8547A control variants;
2. fails closed before and after reset;
3. writes only explicitly supplied generic protection registers;
4. leaves CP disabled and watchdog disabled;
5. enables ADC;
6. reads every written register back;
7. marks init complete only after exact readback.

Stage 3 also exposes masked watchdog control after successful init, but still
has **no writable CP-enable or charge-mode interface**.

### Test gate

After init and before any Stage-4 code is considered device-safe:

1. CP remains disabled;
2. ADC remains functional;
3. no unexpected fault bits are asserted;
4. raw protection registers match the explicitly requested profile;
5. omitted optional registers were not intentionally overwritten;
6. basic PMIC/pmic-glink charging still works.

## Stage 4 - manual single-pump control

Status: interface/test plan documented; source implementation is the next code
stage and must remain development-only until Stage 3 passes real hardware.

Design document:

- `docs/sc8547-stage4-manual-control.md`
- initial design commit: `ffd115b`

Stage 4 adds a **second** opt-in:

```dts
southchip,allow-experimental-cp-enable;
```

It also requires explicit VBUS/VBAT authorization windows. There are no default
voltage windows.

Planned rules:

- `work_mode` changes only `REG09[7]`, only while CP is disabled;
- `cp_enable=1` requires Stage-3 `init_done`;
- supported silicon only;
- valid configured VBUS/VBAT safety window;
- adapter and battery present;
- no blocking thermal/OVP/OCP/UCP/VBUS-error status;
- ADC VBUS/VBAT inside the configured window;
- enable changes only `REG07[7]`;
- after the vendor-derived 500 ms observation interval, `REG06[2]` must report
  switching and no new blocking fault may be present;
- failure immediately clears `REG07[7]`;
- disable remains straightforward and fail-closed.

No USB source negotiation or automatic charging policy exists in Stage 4.
Only one pump is to be tested at a time.

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

1. `60c230c` - Stage 1 variant/snapshot support;
2. `bc553aa`, then `f15fb5a` - Stage 2 protection decode/include fix;
3. Stage 3 source series (`aa3c510`, `29e6c6c`, `17d7037a`) only after Stage
   0-2 data have been reviewed; if cherry-picking the historical series rather
   than a future squashed commit, preserve this order;
4. Stage 4 manual-control commit(s) only after Stage 3 passes the hardware gate;
5. Stage 5 dual-pump coordinator;
6. Stage 6 PD/PPS integration.

Documentation-only commits may be cherry-picked at any time.

Never cherry-pick a later control stage merely because it compiles; each stage
assumes the previous hardware gate has passed.

## Build verification

The repository contains two CI paths:

1. the existing full out-of-tree build against torvalds Linux v7.2;
2. a focused SC8547-only Linux v7.2 `W=1` workflow, added so unrelated
   touchscreen/pogo build failures cannot hide charging-driver status.

The first v7.2 full-module run exposed two SC8547-specific compatibility issues:

- strict `%u` formatting of `FIELD_GET()` results;
- `power_supply_config.of_node` removal in favor of `fwnode`.

Both were fixed in `17d7037a`.

The full-module workflow can still fail on existing touchscreen/pogo warnings or
APIs; those failures are outside the charging workstream. The focused workflow
is the authoritative compile signal for `charging/sc8547.c` while those modules
remain independent.

Focused Linux-v7.2 CI definition commit:

- `4a0df97` - clone Linux v7.2 and build only `sc8547_cp.ko` with `W=1`.

The exact Caihong Linux 7.2 kernel configuration/tree remains the final target
compile environment and must still be tested locally when available.
