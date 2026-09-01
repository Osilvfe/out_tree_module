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
- Focused Linux-v7.2 `W=1` CI is the compile gate for charging changes; the
  repository-wide build may still fail because pogo/touchscreen are separate
  workstreams.

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

The downstream virtual charge-pump layer connects the two devices in parallel,
uses CP index 0 as the main CP, and assigns a nominal 3000 mA input-current
budget to each path. These values are reverse-engineering evidence only and are
not automatic limits/requests in the standalone driver.

## Important silicon compatibility rule

SC8547 and SC8547A share the core charge-pump and ADC data map used by this
port. VBUS/IBUS/VBAT/VOUT/VAC/TDIE data locations and scales are shared in the
Oplus source.

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

The common Oplus SC8547 header describes BAT_OVP (`REG00[5:0]`) as 3500 mV +
25 mV/code. Under that definition code `0x36` decodes to 4850 mV, while some
downstream comments near the project configuration describe 4.65 V.

The common header describes the low nibble of `REG05` as 1200 mA +
300 mA/code. Project code `0x0b` therefore decodes to 4500 mA by that formula,
while some downstream comments describe 3.6 A.

These discrepancies are preserved as evidence rather than silently resolved.
The development driver keeps raw values visible and does not automatically
derive a protection profile from project `ovp_reg`/`ocp_reg` values.

## Stage 0 - baseline telemetry

Status: implemented on `main`, not yet hardware-validated in this development
cycle.

Automatic write:

- ADC enable only (`REG11[7]`).

Features:

- I2C/regmap probe
- device ID
- ADC telemetry
- CP enable/mode/switching state
- status/fault decoding
- `power_supply` read-only reporting

### Test gate

For both `0x6f` devices verify stable probe, plausible primary ID (`0x67`
expected for SC8547A), plausible ADC values, sensible status/fault bits, and no
unexpected CP activity.

## Stage 1 - variant model and register snapshot

Status: implemented on `sc8547-next`.

Source commit:

- `60c230c` - variant model, runtime-ID warning, masked helpers and read-only
  register snapshot.

No new automatic CP/protection write is introduced.

### Test gate

Capture `variant` and `register_dump` for both devices unplugged and with a
normal 5 V source attached.

## Stage 2 - protection model, decode only

Status: implemented on `sc8547-next`.

Source commits:

- `bc553aa` - read-only protection decode;
- `f15fb5a` - explicit bitfield-helper include.

`protection_state` reports raw protection registers together with clearly
labelled vendor-header formula decodes. It does not claim those formulas are
electrically validated on Caihong.

No new automatic write is introduced.

### Test gate

Compare `protection_state` against the same device's `register_dump`, any
available downstream register dump, and unplugged/5-V states.

## Stage 3 - gated controlled hardware init

Status: implemented on `sc8547-next`, Linux-v7.2 build-verified, **not
hardware-validated**.

Source commits:

- `aa3c510` - initial gated reset/init/watchdog controls;
- `29e6c6c` - role-safe profile handling and stricter fail-closed behavior;
- `17d7037a` - Linux v7.2 API/format compatibility.

Documentation:

- `docs/sc8547-experimental-control.md`

Experimental init exists only with:

```dts
southchip,allow-experimental-control;
```

It requires explicit raw protection bytes, keeps CP disabled, performs exact
readback, and exposes watchdog control only after successful init.

### Test gate

After `apply_init`, CP must remain disabled, ADC must remain plausible, written
protection bytes must read back exactly, no unexpected fault may appear, and
normal PMIC/pmic-glink charging must remain functional.

## Stage 4 - manual single-pump control

Status: implemented on `sc8547-next` in source commit `8033a03`, focused
Linux-v7.2 `W=1` CI **passes**, but the stage remains **not hardware-validated**.

Documentation:

- `docs/sc8547-stage4-manual-control.md`
- design commit `ffd115b`

Stage 4 adds a second opt-in:

```dts
southchip,allow-experimental-cp-enable;
```

and requires explicit VBUS/VBAT authorization windows. There are no default
voltage windows.

Implemented rules:

- mode changes only `REG09[7]` and are refused while CP is enabled;
- enable requires Stage-3 `init_done`, supported silicon, complete window,
  adapter/battery presence, no blocking fault and in-window ADC values;
- enable changes only `REG07[7]`;
- after 500 ms the driver requires `REG06[2]` switching state and rechecks
  faults/voltage window;
- failed post-enable validation immediately clears `REG07[7]`;
- disable is fail-closed and intentionally less restrictive;
- no USB source negotiation or automatic fast-charge policy exists.

### Test gate

Primary and secondary must each pass repeated controlled single-pump tests
separately before any dual-pump write path is considered safe.

## Stage 5A - dual-pump pairing and aggregate telemetry

Status: implemented on `sc8547-next`, focused Linux-v7.2 `W=1` CI **passes**,
not hardware-validated.

Architecture/documentation:

- `docs/sc8547-stage5-dual-coordinator.md`
- `1801dae` - align documentation to a separate virtual coordinator node.

Source/build commits:

- `18581b0` - add read-only `sc8547_dual` platform coordinator;
- `ec05245` - build `sc8547_dual.ko`;
- `6986e8c` - require physical primary/secondary role match;
- `edf0998` - focused CI builds both `sc8547_cp.ko` and `sc8547_dual.ko`.

Stage 5A mirrors the downstream virtual-CP layering with a separate DT node:

```dts
charge-pump-coordinator {
    compatible = "southchip,sc8547-dual-experimental";
    southchip,primary = <&cp_primary>;
    southchip,secondary = <&cp_secondary>;
};
```

The coordinator resolves the two I2C clients through phandles, requires them to
be distinct, bound to the `sc8547` physical driver and labelled `primary` /
`secondary`, then exposes only read-only paired telemetry:

- peer identity/ID/variant label;
- per-pump enable/switching/mode/fault and VBUS/VBAT/IBUS snapshot;
- arithmetic aggregate IBUS.

Stage 5A performs no protection/mode/watchdog/CP-enable write.

### Test gate

With both pumps disabled, validate the coordinator node, physical-device
identity, both ADC sets and exact aggregate-IBUS arithmetic unplugged and on a
normal 5 V source. Confirm register snapshots are unchanged merely by loading
and reading `sc8547_dual.ko`.

## Stage 5B - gated dual-pump coordinator

Status: design documented; next source work is to expose a small shared safety
API from the physical driver before adding any coordinator write control.

Rules:

- a third explicit opt-in belongs to the **virtual coordinator node** before
  dual-start controls appear;
- coordinator must call the same physical-driver preflight/enable/post-check/
  disable helpers used by Stage 4 rather than duplicating raw SMBus writes;
- both devices must have Stage-3 init complete and Stage-4 authorization ready;
- both preflights must pass before the first enable write;
- primary (downstream `main_cp = <0>`) starts first and must reach validated
  switching state before secondary starts;
- secondary starts second and must independently pass post-enable validation;
- if secondary fails, disable secondary and primary;
- dual stop disables secondary first, then primary;
- first implementation has no degraded one-pump fallback;
- no PD/PPS/VOOC/UFCS policy is introduced.

The shared physical-driver API refactor itself should preserve Stage-4 behavior
and may be compiled/reviewed independently before the coordinator receives any
new writable attribute.

## Stage 6 - USB-PD/PPS policy integration

Planned only after dual-pump hardware operation is stable.

Goals:

- negotiate source voltage/current before CP start;
- coordinate requested VBUS with battery voltage and 2:1/bypass mode;
- ramp current instead of immediately requesting the final level;
- continuously handle thermal/fault/input-collapse conditions;
- fall back to normal PMIC charging on failure.

## Stage 7 - proprietary protocol work

Optional/later. VOOC/SuperVOOC/UFCS blocks remain separate from generic CP
bring-up and require their own design/review/testing.

## Suggested cherry-pick sequence

When hardware testing becomes available, move changes to `main` in this order:

1. `60c230c` - Stage 1 variant/snapshot support;
2. `bc553aa`, then `f15fb5a` - Stage 2 protection decode/include fix;
3. `aa3c510`, `29e6c6c`, `17d7037a` - Stage 3 source series, only after Stage
   0-2 data review;
4. `8033a03` - Stage 4 manual single-pump control, only after Stage 3 passes on
   hardware;
5. Stage 5A source/build series: `18581b0`, `ec05245`, `6986e8c` (plus relevant
   documentation; CI-only `edf0998` is optional for local cherry-picks);
6. Stage 5B shared-API/coordinator commit(s), only after both single pumps pass
   Stage 4;
7. Stage 6 PD/PPS integration.

Documentation-only and CI-only commits may be cherry-picked earlier.
Never promote a write-capable stage solely because it compiles.

## Build verification

The focused workflow clones torvalds Linux v7.2 and builds the charging modules
with `W=1`. It is the charging workstream's compile gate while unrelated
pogo/touchscreen code remains independent.

- original focused CI definition: `4a0df97`;
- Stage-4 source `8033a03`: `sc8547_cp.ko` focused v7.2 build passed;
- Stage-5A CI update `edf0998`: focused build now checks both
  `sc8547_cp.ko` and `sc8547_dual.ko`;
- Stage-5A head including role validation (`6986e8c`) passed that focused build.

The repository-wide Linux-v7.2 workflow may still fail on unrelated
pogo/touchscreen compilation issues. The exact Caihong Linux 7.2 configuration
and real tablet remain the final target compile/hardware environment.