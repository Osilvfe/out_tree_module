# SC8547 Stage 6 USB-PD/PPS policy boundaries

Stage 6 connects the already-separated charge-pump layers to USB source-contract
state. This stage is more conservative than the earlier local CP controls:
changing a USB-PD/PPS contract changes the electrical input seen by the whole
charging path, not just one SC8547 register.

This document is the design/test contract for Stage 6. Any later source commit
that can request a source voltage/current must update this document and
`sc8547-commit-test-matrix.md` **before** it is considered testable.

## Layering

The intended stack is:

```text
Qualcomm charger firmware / PMIC Glink
        |
        | source contract observation/request
        v
Stage-6 source/policy layer
        |
        | structured CP-pair state and explicit start/stop
        v
sc8547_dual.ko              (virtual two-pump coordinator)
        |
        | shared physical safety API
        v
sc8547_cp.ko x 2            (primary + secondary physical pumps)
```

The SC8547 physical driver must never own USB-PD policy. The virtual dual-pump
coordinator must never fabricate or silently change a USB contract. A later
policy layer may coordinate the two only after the source-contract interface is
proved independently.

## Mainline Linux v7.2 evidence

`drivers/power/supply/qcom_battmgr.c` exposes Qualcomm charger-firmware USB
state as `qcom-battmgr-usb` on the SM8350/SM8550-style path. It reports:

- `POWER_SUPPLY_PROP_ONLINE`
- `POWER_SUPPLY_PROP_VOLTAGE_NOW`
- `POWER_SUPPLY_PROP_VOLTAGE_MAX`
- `POWER_SUPPLY_PROP_CURRENT_NOW`
- `POWER_SUPPLY_PROP_CURRENT_MAX`
- `POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT`
- `POWER_SUPPLY_PROP_USB_TYPE`

Supported USB types include normal PD and `POWER_SUPPLY_USB_TYPE_PD_PPS`.
Current mainline can therefore observe a firmware-reported PD/PPS state and
associated voltage/current values.

Important limitation: the USB `power_supply_desc` has `get_property` but no USB
`set_property` callback. Linux v7.2 therefore has no established public
power-supply interface for this port to request a new PD/PPS contract.

The same mainline driver defines:

```text
BATTMGR_USB_PROPERTY_GET = 0x32
BATTMGR_USB_PROPERTY_SET = 0x33
```

The existence of SET opcode `0x33` is protocol evidence, not permission to
invent extended property IDs or sequencing in an out-of-tree driver.

Current torvalds master also does not provide an upstream
`qcom_battmgr_usb_set_property()` path. Third-party patches that expose generic
USB setters are useful comparison material only; they are not Caihong firmware
ABI proof.

## OnePlus downstream evidence: fixed PD and PPS are different operations

The Caihong downstream charging stack confirms that fixed PD and PPS are not one
interchangeable "set VBUS" operation.

### Fixed PDO path

`OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG` reaches the SM8350/QCOM platform charger
implementation. That implementation:

- accepts a 32-bit PDO;
- accepts only `PD_SRC_PDO_TYPE_FIXED`;
- decodes the fixed-PDO voltage;
- permits 5 V, 9 V or 12 V in that function;
- writes a downstream `BATT_SET_PDO`/`OPLUS_SET_PDO` property containing the
  requested voltage in mV;
- rejects battery, variable and augmented PDOs.

Therefore **BUCK_SET_PD_CONFIG is fixed-PD selection, not PPS control**.

### PPS request path

PPS is exposed separately as `OPLUS_IC_FUNC_PPS_PDO_SET`. On the legacy/non-
SoCCP USB property path it calls the charger backend twice:

```text
USB_SET_PPS_VOLT = requested VBUS in mV
USB_SET_PPS_CURR = requested IBUS in mA
```

The Pad Pro branch's `usb_property_id` enumeration places these at:

```text
USB_GET_PPS_TYPE   = 32
USB_GET_PPS_STATUS = 33
USB_SET_PPS_VOLT   = 34
USB_SET_PPS_CURR   = 35
```

`write_property_id()` builds a charger request with:

```text
property_id = selected property
battery_id  = 0
value       = caller value
owner       = MSG_OWNER_BC
type        = MSG_TYPE_REQ_RESP
opcode      = pst->opcode_set
```

and the USB psy is initialized with:

```text
opcode_get = BC_USB_STATUS_GET = 0x32
opcode_set = BC_USB_STATUS_SET = 0x33
```

So for the **non-SoCCP downstream branch**, the observed request wire model is
`opcode 0x33 + property 34/35 + mV/mA value`.

This is still **not yet a validated Caihong mainline write ABI**. The platform
backend also has a `soccp_support` branch that uses another Oplus property
namespace. The `oplus-chg-23926.dtsi` overlay itself does not set
`oplus,soccp_support`, but an exhaustive check of the effective inherited
`battery_charger` node is still required before selecting the non-SoCCP ABI for
real writes.

## Downstream PPS session/startup evidence

The higher-level `oplus_chg_pps.c` makes clear that PPS is a session/state
machine rather than two independent setters. This sequencing is important to
Stage 6B/6C design.

### 1. Protocol ownership and source-type gate

The PPS worker first asks the charging-protocol arbiter to switch to PPS. It then
requires the current wired charger type to be `PD_PPS` (with a retention retry
path). If the source is not reported as PPS, it ends the PPS protocol switch and
returns without starting fast charge.

This means a future bridge must not infer PPS support merely from accepting a
voltage/current value; the source capability/state gate comes first.

### 2. Optional Oplus-adapter verification

For a recognized PD-SVOOC-ID adapter, downstream calls an adapter verification
routine. Verification failure does not necessarily reject all PPS; it marks the
adapter as non-Oplus and allows the third-party PPS policy path instead.

Therefore proprietary adapter authentication must **not** be made a generic
prerequisite for standards-based PPS support in a mainline design. It affects
which downstream strategy/current limits are chosen.

### 3. APDO capability discovery before any PPS request

Downstream retrieves the source PDO/APDO table and examines only augmented PDOs
for PPS. It rejects the fast-charge path unless:

- the APDO minimum voltage supports the initial low-voltage request (<= 5.5 V);
- the configured target VBUS lies inside the APDO voltage range;
- a usable maximum current can be determined.

The protocol/power arbiter is then informed of the selected maximum power and a
base-current vote is established.

This is stronger evidence that Stage 6B must provide **capability discovery or a
firmware-confirmed equivalent** before exposing a generic PPS request API.

### 4. Charge-allow checks before CP setup

Downstream initializes PPS variables and runs its charge-allow policy before
preparing the CP. That full policy includes product-specific temperature/SOC/
impedance/current constraints that are outside our initial mainline port.

We should not copy those vendor policy tables blindly. The important architectural
point is that source capability and local battery/device safety eligibility are
both checked before CP operation.

### 5. CP ratio/mode prepared while the pump is still not working

Downstream derives a CP work mode from the configured target VBUS, checks that
the CP implementation supports it, and programs that work mode. It then enables
CP ADC, enables a 5 s CP watchdog and determines whether per-CP IBUS monitoring
is available.

This ordering matches the direction of our Stage-3/4/5 split: configure and
observe first, start switching only later.

### 6. Normal wired path is suspended before the initial PPS request

After the CP has been prepared for observation, downstream votes the normal
wired charging path suspended and issues the first PPS request:

```text
PPS_START_DEF_VOL_MV = 5500 mV
start current        = 800 mA for verified Oplus PPS adapter
                     = 1000 mA for third-party PPS adapter
```

The initial request is deliberately low-power. Downstream does **not** jump
straight to the final target voltage/current.

This is a critical Stage-6C design constraint: the first generic mainline policy
must also start from a deliberately conservative contract, not from a maximum-
power target.

### 7. VBUS is raised toward battery-voltage × CP ratio before CP start

After the initial 5.5 V request, a monitor worker repeatedly runs
`oplus_pps_charge_start()`. It computes a target input voltage approximately as:

```text
VBAT × CP ratio + mode/temperature-dependent offset
```

It reads CP input voltage and changes requested PPS VBUS in bounded steps. The
observed startup step sizes include 100 mV, 200 mV, 500 mV, 1 V and 2 V selected
by distance to target. The function waits 500 ms between startup adjustments.

Before each source-voltage adjustment it also programs a conservative CP input
current corresponding to the startup-current floor.

### 8. CP is started only after input voltage is near the required ratio

Only when CP VIN is within the target window does downstream attempt to enable
and start CP work. It then waits/rechecks whether CP is actually working. If CP
fails to start after bounded retries, downstream clears work-start/enable,
disables watchdog/ADC, dumps state and fails the PPS startup.

This confirms the correct causal order for Stage 6C:

```text
source PPS contract established and ramped near required CP input
        -> CP start
        -> switching confirmation
        -> charging/ramp strategy
```

not:

```text
CP start -> hope source voltage catches up later
```

### 9. Current/voltage ramp continues after switching is confirmed

Once CP work is confirmed, downstream marks PPS charging active and starts a
current-work loop. For its Oplus PPS path, requested current is adjusted in
small 100/300 mA steps. Third-party PPS target-voltage logic can make smaller
voltage adjustments (for example 40/100 mV paths) while observing current and
battery/CP state.

We do not need to duplicate these exact policy curves, but the evidence strongly
supports a **bounded-step state machine** rather than direct jumps to final
power.

## Downstream failure/exit evidence

Failure handling is as important as startup.

The downstream force/soft-exit paths first mark PPS charging inactive and then
perform a broad fail-closed teardown that includes:

```text
exit PPS mode
stop CP work
CP disable
CP watchdog disable
CP ADC disable
switch charging path back toward normal
release PPS current votes
release wired-suspend vote
```

The hard/force exit additionally clears PPS online state and tells the protocol
arbiter that PPS has ended. On a protocol-fatal condition, the monitor asks the
arbiter for normal PD when the current protocol is not already PD.

On USB disconnect, downstream force-exits PPS and cancels the monitor/current
workers.

This gives Stage 6B/6C a non-negotiable rollback ordering principle:

1. stop/disable charge-pump load first;
2. end the PPS session / source-specific policy;
3. restore/release the normal wired charging path;
4. verify the resulting basic source state.

The exact Qualcomm firmware operation corresponding to "exit PPS mode" is still
to be confirmed for a future mainline bridge. We must not replace it by merely
sending 5 V/low current unless hardware/firmware tests prove that is equivalent.

## What is now known vs still unverified

### Confirmed from this Pad Pro downstream branch

- Fixed-PDO selection and PPS APDO requests are separate operations.
- Non-SoCCP PPS request properties are 34/35 in this branch.
- Their request units are mV/mA.
- Their USB SET opcode is `0x33`.
- PPS startup checks source type and augmented-PDO capability before requesting
  the initial contract.
- Initial PPS request is 5.5 V at a low current (0.8 A or 1.0 A depending
  downstream adapter classification).
- Source VBUS is ramped toward the CP-required input before CP work starts.
- CP switching is explicitly confirmed before normal PPS charging/ramping.
- Downstream failure paths stop CP and restore the normal charging path rather
  than continuing a partially-started PPS session.

### Still unverified before any Stage-6B write commit

1. Whether Caihong's **effective inherited** charger DT selects `soccp_support`.
2. Whether numeric properties 34/35 are accepted by the firmware running on the
   tablet under the mainline PMIC-Glink stack, or are Oplus-only host ABI
   extensions coupled to the downstream driver.
3. How to obtain the APDO/PDO capability table through a safe mainline-facing
   API; standard `qcom-battmgr-usb` properties do not expose the full APDO list.
4. The exact acknowledgement/error semantics for SET property 34/35 on this
   firmware, including timeout and asynchronous source-transition behavior.
5. The exact firmware operation for entering/exiting PPS mode and whether it is
   distinct from simply setting voltage/current.
6. The correct deterministic return-to-basic/5-V operation under mainline.
7. Behavior when PMIC-Glink/charger firmware service resets during a request or
   active PPS contract.
8. Whether a future upstreamable implementation should extend `qcom_battmgr`,
   expose an internal Qualcomm charger request API, or integrate with another
   Type-C/PD abstraction.

Until these points are resolved, **Stage 6 contains no source-contract write**.

## Stage 6A — read-only source-contract diagnostics

Stage 6A is implemented as a separate diagnostic platform driver above
`sc8547_dual`. It is intentionally observational.

### Development-only DT

```dts
sc8547_policy_diag: charge-policy-diagnostic {
    compatible = "southchip,sc8547-policy-diagnostic";
    southchip,charge-pump = <&sc8547_dual>;
    southchip,usb-power-supply-name = "qcom-battmgr-usb";
};
```

The power-supply name is explicit rather than silently assumed. This is a local
bring-up binding, not an upstream proposal.

### Stage-6A module

```text
sc8547_policy_diag.ko
```

The module consumes a read-only structured API from `sc8547_dual.ko`; it does
not rediscover the two physical SC8547 chips or read their registers directly.

Read-only attributes under `sc8547_policy/`:

- `usb_supply`
- `source_state`
- `combined_state`

`source_state` reports:

```text
online
usb_type (numeric and local text label)
voltage_now_uv
voltage_max_uv
current_now_ua
current_max_ua
input_current_limit_ua
pps_detected
```

`combined_state` correlates that snapshot with the virtual pair state:

```text
primary/secondary initialized + authorized
enabled + switching + mode + fault
primary/secondary VBUS/VBAT/IBUS
aggregate CP IBUS
source USB type/VBUS/current values
```

These are diagnostics, not policy decisions. In particular:

- `VOLTAGE_MAX` is not automatically a requested PPS voltage;
- `CURRENT_MAX` is not permission to draw it;
- `INPUT_CURRENT_LIMIT` is not automatically split between CPs;
- seeing `PD_PPS` does not prove a write API is available;
- aggregate SC8547 IBUS is not battery charge current.

### Stage-6A prohibition

Stage 6A contains none of:

```text
power_supply_set_property()
pmic_glink_write()
BC_USB_STATUS_SET / BATTMGR_USB_PROPERTY_SET
USB_SET_PPS_VOLT / USB_SET_PPS_CURR
fixed-PDO requests
automatic CP enable
```

If any such behavior is introduced, it starts a new write-capable stage and
requires another design/test gate.

## Stage-6A test sequence

Stage 6A is testable with both charge pumps disabled and does not require
Stage-5B dual switching to pass. The Stage-5A pair telemetry must already be
trusted.

### Test 6A.0 — no source

1. Boot with both SC8547s and virtual coordinator present.
2. Omit the Stage-5B dual-write opt-in and keep both CPs disabled.
3. Load the policy diagnostic module.
4. Capture `source_state` and `combined_state` with USB unplugged.
5. Confirm no writable source controls appear and CP register snapshots do not
   change.

### Test 6A.1 — ordinary 5 V source

1. Attach a known ordinary 5 V source.
2. Capture Qualcomm USB power-supply sysfs values directly.
3. Capture `source_state` and `combined_state`.
4. Compare reported source VBUS with both SC8547 VBUS ADC values.
5. Keep CPs off.

This establishes unit/scaling consistency before interpreting PD/PPS data.

### Test 6A.2 — fixed-PD source

Use a known PD adapter/cable and allow the existing Qualcomm firmware/mainline
stack to reach whatever fixed contract it normally selects. Stage 6A itself
must not request one.

Capture USB type, voltage/current properties, both SC8547 VBUS ADCs and CP state.

### Test 6A.3 — PPS-capable source

With a PPS-capable adapter/cable, observe whether Qualcomm firmware reports
`POWER_SUPPLY_USB_TYPE_PD_PPS` and record all source properties while CPs remain
off.

Record adapter/cable identity alongside the direct qcom-battmgr sysfs values and
Stage-6A combined snapshot. This is evidence for Stage 6B, not permission to
send SET messages.

## Stage 6B — future source-request bridge, not implemented

Stage 6B is a separate write-capable stage only after the firmware ABI/session
semantics are confirmed.

The bridge should **not** initially be a generic "set voltage/current" API.
Downstream evidence now suggests the minimum sensible abstraction is a source
session with at least:

```text
observe capabilities/state
enter/prepare PPS session
request PPS voltage/current with bounded ranges
observe/confirm transition
exit PPS session
verify return to basic/normal charging
```

The first hardware tests must keep both SC8547s disabled:

1. observe stable basic/5-V state;
2. prove capability discovery/eligibility;
3. enter PPS using the verified firmware interface;
4. issue one deliberately conservative low-power request;
5. require positive request completion and independently verify VBUS;
6. exit PPS and verify restoration of basic/normal charging;
7. repeat the enter/request/exit cycle with CPs still disabled;
8. exercise request failure and PMIC-Glink/service-reset rollback;
9. only after this may any CP policy consume the bridge.

A source bridge that cannot demonstrably exit and restore a safe/basic state
must never be connected to automatic CP start.

## Stage 6C — future CP/source policy and ramp

Only after Stage 5B and Stage 6B are independently hardware-validated should an
automatic policy be considered.

The first policy must be deliberately limited:

- one already-validated CP ratio;
- conservative initial source current;
- source capability/session confirmed before CP preparation;
- source VBUS ramped near the required `VBAT × ratio` region before CP start;
- CP start followed by explicit switching confirmation;
- gradual voltage/current changes instead of jumps to final power;
- continuous source/CP fault and VBUS/VBAT/IBUS observation;
- reverse-order CP shutdown before source-session fallback;
- no VOOC/SuperVOOC/UFCS mixed into the generic PD/PPS state machine.

The downstream 5.5 V / 0.8–1.0 A startup is useful evidence for conservative
bring-up, **not automatically the final values for our implementation**. Actual
Stage-6B/6C first-test values will be documented only after Stage-6A hardware
captures and firmware-ABI confirmation.

## Merge discipline for Stage 6

The implemented/read-only sequence is:

```text
D  protocol evidence + electrical/test boundary
F  read-only virtual-pair API
F  read-only Stage-6A diagnostic module
F  build integration
C  focused Linux-v7.2 CI integration
D  commit/test matrix + roadmap status
```

Stage 6B starts another documented sequence. Its design/evidence commit must
precede any SET/write source commit. Every Stage-6 commit is recorded in
`sc8547-commit-test-matrix.md` with its meaning, write boundary, prerequisites,
first meaningful test and promotion status.