# SC8547 stage evidence capture tool

Commit `8044c92` adds:

```text
scripts/sc8547-stage-capture.sh
```

The tool is an **observation-only evidence collector** for the staged hardware
plan in `docs/sc8547-stage-test-plan.md`.

It never performs a sysfs/control write. All write-capable Stage-3/4/5B/future
6B operations remain explicit manual commands in the operator test plan.

## Usage

```sh
./scripts/sc8547-stage-capture.sh <stage> [label] [output-dir]
```

Examples:

```sh
./scripts/sc8547-stage-capture.sh 1 unplugged
./scripts/sc8547-stage-capture.sh 2 5v
./scripts/sc8547-stage-capture.sh 3 primary-before-init
./scripts/sc8547-stage-capture.sh 3 primary-after-init
./scripts/sc8547-stage-capture.sh 4 primary-before-enable
./scripts/sc8547-stage-capture.sh 4 primary-enabled
./scripts/sc8547-stage-capture.sh 4 primary-after-disable
./scripts/sc8547-stage-capture.sh 5A unplugged
./scripts/sc8547-stage-capture.sh 5B dual-enabled
./scripts/sc8547-stage-capture.sh 6A pps-adapter
./scripts/sc8547-stage-capture.sh 7C five-minute-pass
./scripts/sc8547-stage-capture.sh 7C active-unplug
```

## Optional test metadata

Before running the collector, set strings that make the evidence directory
self-describing:

```sh
export SC8547_KERNEL_COMMITS='60c230c bc553aa f15fb5a ...'
export SC8547_DT_COMMITS='my-caihong-dt-test-commit'
export SC8547_ADAPTER='adapter model / rated PDO/APDO list'
export SC8547_CABLE='cable model / rating'
export SC8547_NOTES='battery SOC, lab supply notes, anything unusual'
```

They are copied verbatim to `test-context.txt`.

## What is collected

The output directory contains, when available:

- kernel/version/cmdline context;
- `/proc/config.gz`;
- module metadata and SHA-256 for:
  - `sc8547_cp`
  - `sc8547_dual`
  - `sc8547_policy_diag`
- both I2C `0x6f` physical SC8547 sysfs groups;
- visible experimental attributes, **read only**, including each pump's
  `pulse_result` and full `pulse_diagnostics` when present;
- virtual dual-pump state;
- Stage-6A policy diagnostics;
- `qcom-battmgr-usb` source properties;
- the battmgr-side `oneplus_pps_dual_500ms` result when present;
- a complete power-supply inventory including battery terminal voltage, OCV,
  current, capacity and temperature where exported;
- `/sys/class/usb_power_delivery` capability tree;
- Type-C role/revision inventory;
- Type-C power-operation mode, which distinguishes a negotiated PD contract
  (`usb_power_delivery`) from default/Type-C current advertisement;
- runtime DT paths/properties related to battmgr/SoCCP;
- final `dmesg` capture;
- a concise `SUMMARY.txt`.

The USB-PD capability capture is especially important for Stage 6A because it
can show whether Caihong UCSI exposes partner fixed PDOs/PPS APDOs through the
standard Linux USB Power Delivery class.

The runtime-DT capture is especially important before Stage 6B because source
code analysis alone is not enough to prove which Oplus BATTMGR property
namespace the flashed device actually uses.

## Stage-by-stage capture points

### Stage 0

Recommended labels:

```text
unplugged
5v
after-5v-remove
```

Purpose: establish baseline telemetry/basic-charging behavior.

### Stage 1

Recommended:

```text
primary-unplugged
primary-5v
secondary-unplugged
secondary-5v
```

The script currently captures both physical devices in each run; the labels are
still useful for describing the intended electrical state.

### Stage 2

Recommended:

```text
unplugged
5v
```

Purpose: preserve raw register dump beside `protection_state` decode.

### Stage 3

For each pump test session:

```text
primary-before-init
primary-after-init
primary-after-reboot-before-init
primary-after-reboot-after-init
```

then equivalent secondary captures.

The operator still manually runs `apply_init` between before/after captures.

### Stage 4

For each pump:

```text
primary-before-enable
primary-enabled
primary-after-disable
```

Repeat for secondary.

The `enabled` capture should be taken promptly after the driver has completed
its built-in post-enable validation. Do not leave the pump running merely to
collect extra logs.

### Stage 5A

Recommended:

```text
unplugged
5v
wrong-role-negative-test
restored-correct-role
```

The wrong-role capture should show pair refusal while both pumps remain off.

### Stage 5B

Recommended first dual session:

```text
before-dual-enable
dual-enabled
after-dual-disable
```

Take `dual-enabled` immediately after the successful start/snapshot point, then
perform the explicit manual stop from the test plan.

### Stage 6A

Use the source-observation sequence:

```text
unplugged
ordinary-5v
fixed-pd
pps-adapter
```

For `pps-adapter`, `usb-power-delivery.txt` should be reviewed for a partner
`source-capabilities` tree and any `programmable_supply` APDO entries.

Keep both SC8547 pumps off.

### Stage 6B

There is no write-capable Stage-6B implementation yet.

The current collector may already be used as a read-only preflight capture with
label such as:

```text
source-bridge-preflight
```

When the future kernel-tree bridge exists, the operator plan will define
separate captures around fixed-5V fallback and one conservative PPS request.
The collector itself will remain read-only.

### Stage 7C

Stage 7C remains a manually triggered, five-minute maximum experiment. Run the
collector after the manual trigger has returned; it reads but never writes the
`oneplus_pps_dual_500ms` endpoint. Recommended labels for the next lifecycle
tests are:

```text
five-minute-pass
active-unplug
replug-before-restart
replug-five-minute-pass
manual-stop
```

The first stable C19 run and its post-prepare recovery are already qualified.
The next priority is detach/replug lifecycle behavior, not higher power, full
charge, automatic attach start, suspend or an intentionally heated run.

For an active-unplug test, collect only after the write has returned. A safe
result must show both pumps off, successful cleanup and a low-current or
USB-offline termination. The latest two-second `usb_online` monitor value may
still be the pre-detach sample when a 500-ms pump guard detects the collapse
first. Treat that ordering as expected, and use physical `pulse_diagnostics`
to distinguish it from an unexplained pump stop.

After reconnecting, take `replug-before-restart` before issuing another manual
start. Both pumps must be off, the USB supply must be online again, and no run
may start automatically. Then run one ordinary five-minute test and collect
`replug-five-minute-pass`; this checks that detach cleanup left no stale pump,
profile or battmgr state.

Full-charge qualification and automatic sustained charging remain explicitly
out of scope.

### Stage 7D

Stage 7D starts with a manually triggered ten-minute endurance gate. It keeps
the same `oneplus_pps_dual_500ms` endpoint and all Stage-7C electrical and
lifecycle guards; only the bounded hold and physical worker windows are longer.
Use the label `ten-minute-pass` after a natural-duration run. A passing D0
capture has 1200 hold samples, 301 two-second system checks, successful physical
completion and zero off-guard, cleanup and fixed-5-V errors. Manual stop remains
available and must continue to report `-ECANCELED`.

This stage still has no automatic attach start and does not qualify full-charge,
suspend or heated operation.

## Before/after comparison rule

For write-capable stages, keep separate directories rather than overwriting one
capture. Example:

```text
sc8547-stage-4-primary-before-enable-...
sc8547-stage-4-primary-enabled-...
sc8547-stage-4-primary-after-disable-...
```

This preserves the exact register/fault/source state at each transition.

## Passing the evidence onward

When reporting a failed stage, provide the complete output directories from the
last known-good state and the first failed state. Do not cherry-pick the next
Stage before those are reviewed.

The most useful minimum pair for debugging is:

```text
last-good capture
first-bad capture
exact kernel/DT commit lists
full dmesg
```

## CI contract

The focused charging workflow validates the script with:

```sh
sh -n scripts/sc8547-stage-capture.sh
```

in addition to compiling the three charging modules against Linux v7.2 with
`W=1`.

A script syntax failure therefore blocks the focused charging CI just like a
module compile failure.
