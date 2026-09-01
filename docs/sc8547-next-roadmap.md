# SC8547 development branch roadmap

This document tracks forward development of the OnePlus Pad Pro (`caihong`)
dual-SC8547 charge-pump port.

`main` is conservative/test-oriented. Ongoing work is on `sc8547-next`.
The detailed per-commit meaning, dependency and hardware-test order lives in:

```text
docs/sc8547-commit-test-matrix.md
```

Do not merge the development branch wholesale. Build a hardware-test branch by
cherry-picking only the functional track whose previous gate has already passed.

## Global rules

- Every new electrical capability is isolated in its own commit/small series.
- Write-capable paths require explicit development-only opt-ins.
- Compilation is necessary, never sufficient, for promotion.
- SC8547 physical safety logic stays in the physical driver.
- Dual-pump coordination stays in the virtual coordinator.
- USB source-contract observation/request stays in a layer above the CP pair.
- Fixed PD, PPS, VOOC/SuperVOOC and UFCS are distinct protocol/control paths.
- Ambiguous vendor threshold comments are evidence, not validated limits.
- Focused torvalds Linux-v7.2 `W=1` CI is the charging compile gate.

## Hardware topology

Caihong downstream uses two `0x6f` devices on different I2C buses:

- hub 2: primary SC8547A, downstream CP index 0;
- hub 0: secondary SC8547-family, downstream CP index 1.

The downstream virtual CP connects them in parallel and declares `main_cp = <0>`.
Nominal 3000 mA-per-path values are recorded as downstream evidence only; the
standalone driver never treats them as an automatic current request.

## Stage 0 — baseline telemetry

Status: implemented on `main`; hardware validation still required in this
current bring-up cycle.

Normal-node automatic write: ADC enable only.

Gate: both chips probe, ID/role/ADC/status/faults are plausible, no unexpected
CP activity, and pmic-glink/basic charging remains functional.

## Stage 1 — variant model + register snapshot

Status: implemented.

Functional commit:

```text
60c230c
```

Read-only gate: capture both variants/register dumps unplugged and at normal
5 V.

## Stage 2 — protection decode, read-only

Status: implemented.

```text
bc553aa
f15fb5a
```

Gate: compare raw protection bytes and labelled header-formula decodes against
register dump/downstream evidence. Do not promote vendor comments into physical
limits where the source disagrees with itself.

## Stage 3 — gated controlled initialization

Status: implemented and focused-v7.2 build-verified; **not hardware-validated**.

```text
aa3c510
29e6c6c
17d7037
```

Requires:

```dts
southchip,allow-experimental-control;
```

and an explicit raw profile. `apply_init` leaves CP disabled and verifies
programmed bytes.

Gate: test one pump at a time; exact readback, CP off, plausible telemetry, no
new fault, normal/basic charging still functional.

## Stage 4 — gated manual single-pump switching

Status: implemented (`8033a03`) and focused-v7.2 build-verified;
**not hardware-validated**.

Second opt-in:

```dts
southchip,allow-experimental-cp-enable;
```

with explicit VBUS/VBAT authorization windows.

Enable performs preflight, masked enable, 500 ms observation, switching/fault/
window validation, and fail-closed disable on failure.

Gate: primary and secondary must each pass repeated independent tests before any
dual-start test.

## Stage 5A — read-only virtual pair

Status: implemented and focused-v7.2 build-verified; hardware validation
pending.

Current shared-API functional chain is documented as Track D in the commit/test
matrix. The virtual node identifies primary/secondary by phandle and exposes
paired state/aggregate IBUS without enabling either pump.

Gate: both pumps off; verify pair identity, both state snapshots, aggregate IBUS
and no register changes from coordinator reads.

## Stage 5B — gated dual-pump switching

Status: implemented (`25d0e27`) and focused-v7.2 build-verified;
**not hardware-validated**.

Third opt-in on the virtual node:

```dts
southchip,allow-experimental-dual-cp;
```

Implemented behavior:

- both physical Stage-4 preflights before first enable;
- primary starts/validates first;
- secondary starts only after primary validates;
- failure rolls back secondary then primary;
- explicit stop is secondary then primary;
- no degraded one-pump fallback;
- no USB source negotiation.

Gate: test only after both pumps independently pass Stage 4.

## Stage 6A — read-only USB source-contract diagnostics

Status: **implemented and focused Linux-v7.2 `W=1` build-verified**;
**not hardware-validated**.

Design/test contract:

```text
docs/sc8547-stage6-pd-pps-policy.md
```

Per-commit details/test construction:

```text
docs/sc8547-commit-test-matrix.md   (Track F)
```

Stage-6A commits:

```text
a48fa1f  document PD/PPS architecture and Stage-6 gates
e16d7c2  declare read-only virtual-pair state API
fc32392  export read-only sc8547_dual_get_state()
9ec6520  add read-only source/CP diagnostic driver
756acee  build sc8547_policy_diag.ko
97892e6  build all three charging modules in focused CI
```

Development-only diagnostic DT:

```dts
sc8547_policy_diag: charge-policy-diagnostic {
    compatible = "southchip,sc8547-policy-diagnostic";
    southchip,charge-pump = <&sc8547_dual>;
    southchip,usb-power-supply-name = "qcom-battmgr-usb";
};
```

Read-only diagnostic attributes:

```text
sc8547_policy/usb_supply
sc8547_policy/source_state
sc8547_policy/combined_state
```

The module observes Qualcomm USB `power_supply` state and correlates it with the
dual-CP snapshot. It has no writable sysfs attributes and contains no
`power_supply_set_property()`, Glink SET/write, fixed-PDO request, PPS request or
automatic CP enable.

### Stage-6A hardware gate

Stage 6A is intentionally testable **without enabling Stage 5B**. Omit
`southchip,allow-experimental-dual-cp` and keep both pumps off.

Run in this order:

1. F0: unplugged observation;
2. F1: known ordinary 5 V source; establish unit/scaling consistency between
   qcom-battmgr VBUS and both SC8547 ADCs;
3. F2: fixed-PD-capable source, but allow existing Qualcomm firmware/mainline
   behavior to choose its own contract; Stage 6A only observes;
4. F3: PPS-capable source, observe whether USB type becomes `PD_PPS` naturally
   and capture complete Qualcomm + CP telemetry.

These captures are evidence for Stage 6B. They do not authorize SET messages.

## Stage 6B — Qualcomm source-request bridge

Status: protocol research only; **no write code exists**.

Current evidence:

- mainline `qcom_battmgr` exposes USB ONLINE/VBUS/current/limit/type read-only;
- its USB types include PD and PD_PPS;
- the firmware protocol defines USB GET `0x32` and SET `0x33`, but mainline USB
  `power_supply` has no SET callback;
- current torvalds master still has no upstream `qcom_battmgr_usb_set_property()`;
- OnePlus downstream Fixed-PD and PPS requests are separate operations;
- downstream `BUCK_SET_PD_CONFIG` accepts **fixed PDO only** and limits that path
  to 5/9/12 V;
- downstream PPS uses separate `USB_SET_PPS_VOLT` and `USB_SET_PPS_CURR`
  properties through USB SET opcode `0x33`;
- third-party hacks that write generic qcom-battmgr USB properties are not
  treated as Caihong firmware-ABI proof.

Next research requirements before any Stage-6B functional commit:

1. establish the exact Caihong charger-firmware property IDs and units for PPS;
2. determine whether they are Qualcomm ABI or Oplus-only extension IDs;
3. confirm required PPS enable/auth/capability sequencing;
4. confirm ack/success semantics and source-state update latency;
5. define a deterministic return-to-basic/5-V operation;
6. test any future source bridge with **both SC8547s disabled first**.

No source-write commit should be created before these items are documented.

## Stage 6C — future automatic source/CP policy

Blocked on independent Stage-5B and Stage-6B hardware validation.

First implementation, when eventually allowed, should use one already validated
CP ratio, conservative current, confirmed source contract before CP start,
gradual ramping, continuous source/CP observation and reverse-order CP shutdown
before source fallback.

## Stage 7 — proprietary protocols

VOOC/SuperVOOC/UFCS remain separate optional/later workstreams. They must not be
silently folded into generic PD/PPS bring-up.

## Focused build status

The charging CI currently builds, against torvalds Linux v7.2 with `W=1`:

```text
sc8547_cp.ko
sc8547_dual.ko
sc8547_policy_diag.ko
```

Stage 4, Stage 5A, Stage 5B and Stage 6A all pass this focused compile gate.
The exact Caihong Linux 7.2 tree/configuration and real tablet remain the final
compile/electrical environment.