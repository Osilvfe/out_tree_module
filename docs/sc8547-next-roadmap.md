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
- Every functional stage is kept as a separate commit or small commit series.
- Write-capable behavior is hidden behind explicit development-only DT opt-ins.
- VOOC/SuperVOOC/UFCS remains separate from generic CP work.
- Documentation is written before or together with write-capable stages.
- Focused Linux-v7.2 `W=1` CI is the charging compile gate; repo-wide failures
  from the independent pogo/touchscreen workstreams do not redefine charging
  status.

## Hardware topology

Caihong downstream uses two SC8547-family devices at I2C address `0x6f` on
separate buses:

- hub 2: primary SC8547A, downstream `oplus,sc8547a`, CP index 0;
- hub 0: secondary SC8547-family, downstream `slave_vphy_sc8547`, CP index 1.

Downstream virtual CP connects them in parallel, uses `main_cp = <0>` and lists
a nominal 3000 mA input-current budget for each path. The standalone port treats
those values as evidence only, never as an automatic source request or generic
safe limit.

Both downstream physical nodes carry:

```dts
ocp_reg = <0xb>;
ovp_reg = <0x36>;
```

These project values are not programmed automatically because vendor comments
and header formulas are not fully self-consistent.

## Silicon compatibility rule

SC8547 and SC8547A share the core ADC/data map used by this port, but are not
control-register identical. In particular, REG05 UCP deglitch and downstream
REG07 low-bit setup differ. Shared controls therefore use masked writes, while
variant-specific fields remain separated.

SC8547A also exposes UFCS registers at `0x40+`; those are outside the generic CP
bring-up. SC8547D is recognized for telemetry but rejected by experimental
write paths until its control compatibility is established.

## Protection-value caution

The Oplus common header describes BAT_OVP as 3500 mV + 25 mV/code, making code
`0x36` decode to 4850 mV, while nearby vendor comments can describe 4.65 V.
Similarly, the header formula makes IBUS OCP code `0x0b` decode to 4500 mA while
some source comments describe 3.6 A.

Raw values are therefore preserved beside clearly labelled `header_*` decodes;
ambiguous formulas are never treated as electrically validated board limits.

## Stage 0 - baseline telemetry

Status: implemented on `main`, hardware validation pending.

Automatic write: ADC enable only (`REG11[7]`).

Features include I2C/regmap probe, ID, ADC telemetry, CP state/mode/status,
fault decoding and basic read-only `power_supply` reporting.

### Hardware gate

Verify both `0x6f` devices probe, IDs/ADC/status are plausible and no unexpected
CP activity occurs.

## Stage 1 - variant model and snapshot

Status: implemented on `sc8547-next`.

- `60c230c` - variant model, runtime-ID warning, masked helpers and read-only
  register snapshot.

No new automatic write.

### Hardware gate

Capture `variant` and `register_dump` for both pumps unplugged and on a normal
5 V source.

## Stage 2 - protection decode, read-only

Status: implemented.

- `bc553aa` - read-only `protection_state` decode;
- `f15fb5a` - explicit bitfield include.

### Hardware gate

Compare raw/decoded values against register dump, downstream evidence and
unplugged/5-V states. Keep contradictory formulas labelled ambiguous.

## Stage 3 - gated controlled init

Status: implemented, focused Linux-v7.2 build-verified, **not hardware-validated**.

- `aa3c510` - gated reset/init/watchdog controls;
- `29e6c6c` - role-safe profile handling/fail-closed tightening;
- `17d7037a` - Linux v7.2 API/format compatibility.

Documentation: `docs/sc8547-experimental-control.md`.

Requires:

```dts
southchip,allow-experimental-control;
```

and explicit raw protection bytes. Init leaves CP off, verifies readback and
only then marks `init_done`.

### Hardware gate

After init, CP stays off, ADC remains plausible, written bytes read back exactly,
no unexpected fault appears and pmic-glink/basic charging still works.

## Stage 4 - manual single-pump control

Status: implemented in `8033a03`, focused Linux-v7.2 `W=1` passes,
**not hardware-validated**.

Documentation: `docs/sc8547-stage4-manual-control.md` (`ffd115b` design commit).

Requires a second opt-in and explicit VBUS/VBAT authorization windows:

```dts
southchip,allow-experimental-cp-enable;
```

Mode changes are masked and allowed only while off. Enable repeats Stage-4
preflight, sets REG07[7], waits the vendor-derived 500 ms observation interval,
requires switching and rechecks faults/voltage window. Failure clears enable.

### Hardware gate

Primary and secondary must each pass repeated controlled single-pump tests
independently before Stage 5B is considered safe to test.

## Stage 5A - virtual pair and aggregate telemetry

Status: implemented, focused Linux-v7.2 `W=1` passes, hardware validation
pending.

Architecture/documentation: `docs/sc8547-stage5-dual-coordinator.md`.

Initial read-only source/build series:

- `18581b0` - read-only `sc8547_dual` platform coordinator;
- `ec05245` - build `sc8547_dual.ko`;
- `6986e8c` - validate primary/secondary roles;
- `edf0998` - focused CI builds both charging modules.

The coordinator is a separate virtual node, matching the downstream virtual-CP
layering concept:

```dts
charge-pump-coordinator {
    compatible = "southchip,sc8547-dual-experimental";
    southchip,primary = <&cp_primary>;
    southchip,secondary = <&cp_secondary>;
};
```

Later refactor `7caaee0` moved Stage-5A state reads onto the shared physical
API, so the virtual layer no longer duplicates SC8547 register interpretation.

### Hardware gate

With both pumps off, verify peer identity, both snapshots, aggregate IBUS and
that loading/reading the virtual module causes no physical register changes.

## Stage 5B - gated dual-pump coordinator

Status: **implemented and focused Linux-v7.2 `W=1` build-verified**, but
**not hardware-validated** and must remain on `sc8547-next` until both pumps pass
Stage 4 independently.

Documentation: `docs/sc8547-stage5-dual-coordinator.md`.

Shared physical API series:

- `60cb97e` - introduce `charging/sc8547_api.h`;
- `e854be9` - keep the API opaque from physical private structures;
- `9b29a00` - physical driver exports shared state/mode/preflight/enable/disable
  helpers and Stage-4 sysfs uses the same safety implementation;
- `3cbdb91` - document the shared API contract;
- `7caaee0` - virtual telemetry consumes the shared state API.

Coordinator write implementation:

- `25d0e27` - add gated dual work-mode and dual enable/disable coordination;
- `0c92c3a` - document the implemented Stage-5B test/rollback contract.

Writable virtual controls require the third opt-in:

```dts
southchip,allow-experimental-dual-cp;
```

Rules implemented:

- `work_mode` updates primary then secondary through the physical API while both
  are initialized/authorized/off; failure before enable performs best-effort
  mode rollback;
- dual start requires both initial states ready and modes matching;
- both physical Stage-4 preflights pass before the first enable;
- primary starts first and completes its own 500 ms post-enable validation;
- secondary starts only after primary is validated;
- secondary/final validation failure disables secondary then primary;
- explicit stop disables secondary then primary and attempts both even if the
  first returns an error;
- virtual driver remove/shutdown performs best-effort dual stop when Stage-5B
  opt-in is present;
- no degraded one-pump fallback in the first implementation;
- no PD/PPS/VOOC/UFCS policy.

Focused CI for `25d0e27` passed building both `sc8547_cp.ko` and
`sc8547_dual.ko` against torvalds Linux v7.2 with `W=1`.

### Hardware gate

Do not test dual start until each pump passes Stage 4. Then test one already
validated ratio with a controlled source: verify primary validated switching
precedes secondary, both are fault-free/in-window after start, and stop returns
both to off. Test refusal paths by removing authorization, not by deliberately
creating electrical OVP/OCP/thermal faults.

## Stage 6 - USB-PD/PPS policy integration

Status: research/design next. No automatic policy code should be written until
we know which mainline Qualcomm/USB-C interfaces on Caihong can safely request
and observe PD/PPS contracts.

Planned work:

1. inventory current mainline `pmic-glink`, USB Type-C/TCPM and power-supply
   interfaces available on SM8650/Caihong;
2. determine whether PPS contract requests are exposed in-kernel or require a
   missing platform bridge;
3. define source-contract ownership so SC8547 code never changes VBUS without
   a confirmed source contract;
4. define a low-current/low-voltage ramp state machine with explicit fallback
   to normal PMIC charging;
5. keep the policy in a separate layer above the physical and virtual CP
   drivers;
6. only after design/compile review consider development-only automatic policy
   code.

## Stage 7 - proprietary protocol work

Optional/later. VOOC/SuperVOOC/UFCS remains a separate protocol layer with its
own review and test gates.

## Suggested cherry-pick sequence

When hardware testing is available:

1. `60c230c` - Stage 1;
2. `bc553aa`, `f15fb5a` - Stage 2;
3. `aa3c510`, `29e6c6c`, `17d7037a` - Stage 3 after Stage 0-2 review;
4. `8033a03` - Stage 4 only after Stage 3 hardware pass;
5. Stage-5A read-only series `18581b0`, `ec05245`, `6986e8c` if you want the
   original no-API telemetry checkpoint;
6. for the current shared-API/Stage-5B line: `60cb97e`, `e854be9`, `9b29a00`,
   `3cbdb91`, `7caaee0`, then `25d0e27` (plus documentation as desired);
7. do not cherry-pick `25d0e27` to `main` until both single pumps pass Stage 4;
8. Stage 6 only after dual hardware validation.

Temporary one-shot refactor tooling used to generate `9b29a00` was removed and
is not part of the recommended cherry-pick set. CI-only commits are optional
for a local test branch.

## Build verification

The focused workflow clones torvalds Linux v7.2 and builds both charging
modules with `W=1`:

```text
sc8547_cp.ko
sc8547_dual.ko
```

Stage 4, Stage 5A and current Stage 5B all pass this focused compile gate. The
repo-wide workflow can still fail on independent pogo/touchscreen code and is
not the charging workstream's status signal.

The exact Caihong Linux 7.2 configuration and real tablet remain the final
compile/hardware environment.