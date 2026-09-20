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

### Caihong runtime caveat

On Caihong, a known PD-capable source produced
`power_operation_mode=usb_power_delivery` through PMIC-Glink UCSI while
`qcom-battmgr-usb/usb_type` still selected `SDP`.  The UCSI result proves that
a PD contract exists, so the upstream SM8350 USB-type property mapping is not a
reliable PD/PPS gate with this OnePlus charger firmware.  Stage 6 must use UCSI
contract state and partner PDO/APDO capability evidence for protocol gating;
the battmgr voltage/current fields remain useful telemetry.

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

## Stage 6B — gated source-request bridge under hardware validation

The local Caihong kernel now contains fixed-value Stage-6B development controls.
The read-buffer, fixed-5-V fallback and 5.5-V/1-A requests have been validated
on hardware with both SC8547s off.  Arbitrary requests and automatic pump
control remain prohibited.

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

Stage-6B hardware tests keep both SC8547s disabled:

1. observe stable basic/5-V state;
2. prove capability discovery/eligibility;
3. enter PPS using the gated firmware interface;
4. issue one deliberately conservative low-power request;
5. require positive request completion and independently verify VBUS;
6. exit PPS and verify restoration of basic/normal charging;
7. repeat the enter/request/exit cycle with CPs still disabled;
8. exercise request failure and PMIC-Glink/service-reset rollback;
9. only after this may any CP policy consume the bridge.

A source bridge that cannot demonstrably exit and restore a safe/basic state
must never be connected to automatic CP start.  The next test ramps only the
source from 5.5 V to 9.0 V in 0.5-V steps at 1 A, then requires an explicit
verified fixed-5-V fallback; it still cannot enable a pump.

## Stage 6C0 — asynchronous single-primary policy

Stage 6C0 is now implemented as a development-only policy in the Caihong
`qcom_battmgr` branch. It is intentionally narrower than the eventual dual-pump
policy and does not require Stage 5B: the secondary pump has not passed its
individual switching gate, so Stage 6C0 requires both pumps to be off and only
starts the already-validated primary.

The DT opt-in is:

```text
oneplus,pps-policy-2to1-1a
```

The resulting device attribute is:

```text
oneplus_pps_policy_2to1_1a
```

Writing `1` queues an asynchronous policy run and returns immediately. Writing
`0` requests cancellation. A run is capped at 15 minutes and retains all of
the earlier electrical bounds:

- 2:1 mode only;
- one primary SC8547A only; secondary must be present and off;
- source request capped at 1 A;
- advertised fixed 5 V and compatible PPS APDO checked before source ramp;
- gauge-derived target and bounded source ramp completed before CP start;
- physical driver confirms start and samples the CP every 500 ms;
- physical checks cover switching/fault state, VAC/VBUS/VBAT, 2:1 error, IBUS
  and die temperature;
- policy checks USB attachment, charger-service availability, gauge VBAT and
  battery temperature every 2 seconds;
- policy VBAT stop is 4.30 V and the initial battery-temperature window is
  10.0–45.0 degrees C;
- every exit completes pump disable/profile restoration before the source is
  requested back to fixed 5 V.

The policy deliberately holds the requested current at 1 A during this first
stage. Dynamic current optimization and dual-pump operation remain prohibited.
Initial PPS elevation is still gradual; changing the settled source contract
while the primary is active is deferred until this lifecycle and rollback stage
has passed hardware testing.

The physical driver remains policy-free. Its only Stage-6C0 addition is a
lock-free cancellation flag checked by the existing bounded run. Cancellation
does not write a register directly; within one 500-ms sample it enters the same
tested disable/readback/restore path used by Stage 4AV.

### Stage-6C0 hardware gates

Test in this order:

1. start the asynchronous policy, observe `state=active`, then request a user
   stop after at least 60 seconds;
2. require `reason=user`, policy `rc=0`, pump `cp_run_rc=-ECANCELED`, all cleanup
   return codes zero and both physical pumps off;
3. run again and allow either the 15-minute duration or the 4.30-V policy limit
   to stop it; both are controlled completions;
4. repeat the physical cable-detach test while active; a negative operational
   result is expected, but pump cleanup and final off-state are mandatory;
5. do not proceed to dynamic source retargeting or dual-pump policy until these
   normal and fault exits are repeatable.

## Stage 6C1 — low-current UCP/soft-start-timeout correction

The first Stage-6C0 natural hardware run stopped after about 79 seconds even
though VBUS/VBAT ratio, die temperature, USB attachment and service telemetry
were valid. The pre-restore reject dump identified the actual transition:

```text
REG05=1b REG08=e0 REG09=34 REG0F=04 REG07=00
```

`REG0F[2]` is the IBUS UCP-fall flag and `REG09[5]` is its rise flag; neither is
a watchdog flag. The vendor low-current helper disables UCP and the coupled
REG08 soft-start timeout until measured input current exceeds 600 mA. Since the
fixed 1-A validation policy has measured only about 0.2--0.4 A, Stage 6C1 uses
the same `REG05=9b`, `REG08=00` state throughout this deliberately low-power
run. IBUS OCP, voltage protections, the watchdog and both software monitor
loops remain enabled.

Stage 6C1 crossed the former UCP failure point and remained switching through
119 seconds, proving the low-current correction. It then rejected one
pump-local `VBAT=4478750` uV sample while qcom-gauge remained near 4.164 V. The
single impossible sample produced a 617500-uV 2:1 ratio error and exposed that
the ADC high and low bytes were still fetched through separate I2C transfers.

## Stage 6C2 — coherent ADC read and numeric-range confirmation

Stage 6C2 keeps the C1 low-current protection state and does not widen an
electrical limit. Each ADC high/low pair is now fetched with one continuous I2C
read. If pump status and fault registers remain healthy but a numeric software
window rejects the sample, the driver takes two additional samples 10--20 ms
apart. A recovered transient is counted; three consecutive out-of-range
samples still stop and restore the pump. Hardware/status faults, I2C errors and
cancellation retain immediate fail-closed behavior. The policy also exposes
`state=stopping` as soon as the physical run returns, before slower monitor and
source cleanup completes.

Both C2 hardware gates passed. Explicit cancellation after approximately three
minutes produced 359 valid 500-ms samples, no ADC range recheck, expected
physical `-ECANCELED`, zero cleanup/restore errors and final pump-off state. The
natural run then completed all 1800 samples with policy `reason=duration`, no
ADC range recheck, valid voltage/current/temperature ranges and clean fixed-5-V
fallback.

## Stage 6C3 — one bounded active PPS step

C3 isolates the next untested boundary without implementing dynamic tracking.
At monitor check 15, it issues exactly one `+20 mV` source request at the same
1-A ceiling, then waits 500 ms and requires the primary pump to remain enabled
and switching. The new request must remain within the advertised APDO and the
existing 9.4-V source ceiling. Pre/post pump-side VBUS and the request result are
retained in one concise policy line. A request or verification failure cancels
the physical run and restores fixed 5 V before reporting `source-retarget`.

Both C3 gates passed. The cancellation gate requested `8910 -> 8930` mV at
monitor check 15, retained valid pump-side VBUS and switching after 500 ms,
then continued through 374 physical samples before clean cancellation and
fixed-5-V fallback. The duration gate requested `8890 -> 8910` mV, completed
all 1800 physical samples with no ADC range recheck, and restored the pump and
source cleanly. Its observed IBUS remained below both the 1-A source request
and 1.2-A physical monitor ceiling; die temperature remained at or below 38
degrees C.

## Stage 6C4 — three bounded active PPS steps

C4 isolates repeated active source requests without adding a feedback policy.
It performs exactly three `+20 mV` requests at monitor checks 15, 30 and 45,
for a maximum cumulative increase of 60 mV. Each request retains the C3 APDO
and 9.4-V bounds, waits 500 ms, rereads pump-side VBUS and requires the primary
pump to remain enabled and switching. The current request stays at 1 A and the
secondary remains off.

The compact policy report records attempted/completed counts, the first source
voltage, final requested voltage, last pre/post VBUS and the last result. Any
failed request or post-request pump check immediately cancels the physical run
and enters the already-tested fixed-5-V fallback. C4 does not derive a target
from gauge or pump ADC values and issues no further source request after the
third step.

The first hardware gate is a run long enough to complete all three steps,
followed by explicit cancellation and full off-state verification. A natural
duration run follows only after that gate passes. Gauge-driven source tracking
is deferred to C5 or later. Dual-pump policy additionally requires both
physical pumps to pass their individual Stage-4 tests and Stage 5B to pass
independently. No Stage 6C variant may mix VOOC/SuperVOOC/UFCS into this generic
PD/PPS state machine.

The C4 cancellation gate passed. All three active requests completed and moved
the source from 8990 to 9050 mV; the last 500-ms pump-side confirmation remained
valid. The pump then continued through 357 valid samples before explicit
cancellation, with no ADC range recheck and with clean pump/profile/source
rollback. The duration gate also passed: all three requests moved the source
from 8970 to 9030 mV, the physical run completed all 1800 samples with no ADC
range recheck, IBUS remained below 522 mA, die temperature remained at or below
37.5 degrees C, and all cleanup/fixed-5-V results were zero.

## Stage 6C5 — one bounded active PPS down-step

C5 validates the remaining request direction before bidirectional feedback is
introduced. At monitor check 15 it requests exactly one `-20 mV` source step,
then uses the same 500-ms pump-side VBUS, enable and switching confirmation as
C3/C4. It requires the target to remain within the advertised APDO and existing
source limits. The source-current request remains 1 A, the primary is the only
active pump, and every electrical and software monitor limit is unchanged.

There is no second request and no feedback decision in C5. Any request or
post-request verification error cancels the physical run and restores fixed 5
V. The first hardware gate observes at least one minute after the down-step and
then explicitly cancels; natural duration follows only if cancellation and all
cleanup paths pass. Current-driven bidirectional feedback remains deferred to
C6 or later.

The C5 natural duration gate passed. The active request moved the source from
9010 to 8990 mV and retained valid pump-side VBUS and switching after 500 ms.
All 1800 physical samples completed without ADC range recheck; pump/profile
cleanup and fixed-5-V fallback all returned zero. An earlier attempt had failed
before pump startup with `-ENODEV` and no monitor/down-step activity; replugging
the source restored the qcom online state and the same image then passed.

## Stage 6C6 — three bounded IBUS-feedback decisions

C6 connects the separately validated up/down request paths to a deliberately
limited current decision. At monitor checks 15, 30 and 45 it reads the primary
SC8547 IBUS ADC. With a 1-A target and a 100-mA deadband, it requests `+20 mV`
below 0.9 A, `-20 mV` above 1.1 A, or holds the source request between those
thresholds. There are exactly three decisions, so at most three source writes
and at most 60 mV cumulative movement are possible.

Every actual source write retains APDO/9.4-V bounds and the 500-ms VBUS plus
enable/switching confirmation. A hold performs no source write but still
requires the pump to be active. The source request remains capped at 1 A, the
physical IBUS software limit remains 1.2 A, the secondary remains off, and all
existing battery/source/temperature cleanup rules are unchanged. This stage is
not a continuous tracker and does not increase requested current.

The C6 cancellation gate passed. All three IBUS decisions selected `+20 mV`
from measured current below 0.9 A; all three requests completed and moved the
source from 9010 to 9070 mV. The pump continued through 270 valid samples before
explicit cancellation, with no ADC range recheck and clean pump/profile/source
rollback. The natural duration gate also passed: three requests moved the
source from 9110 to 9170 mV, all 1800 samples completed without ADC range
recheck, IBUS remained below 269 mA, die temperature remained at or below 37.5
degrees C, and all cleanup/fixed-5-V results were zero.

## Stage 6C7 — six bounded IBUS-feedback decisions

C7 changes only the decision-count ceiling. It retains C6's 30-second cadence,
1-A target, 100-mA deadband and 20-mV bidirectional step, but allows six
decisions at monitor checks 15, 30, 45, 60, 75 and 90. Thus at most six source
writes and 120 mV cumulative movement are possible. APDO/9.4-V bounds,
post-write pump confirmation, the 1.2-A physical IBUS stop, single-primary
topology and all cleanup rules are unchanged. Continuous feedback and higher
requested current remain deferred.

The C7 natural duration gate passed. All six low-IBUS decisions selected
`+20 mV`, all six requests completed the exact 9010-to-9130-mV movement, and
the physical run completed all 1800 samples without an ADC range recheck.
Observed IBUS was 22.5--495 mA and die temperature was 36--38 degrees C. Policy
completion, pump/profile cleanup and fixed-5-V restoration all returned zero.

## Stage 6C8 — conservative 1.25-A source-current request

C8 changes one electrical input after C7 passed: the policy's PPS current
request rises from 1.0 to 1.25 A. The existing manual `*_1a` diagnostics remain
at 1 A; only the policy-owned ramp and its active feedback requests use the new
current. The same APDO check must explicitly admit 1.25 A before any ramp.

The six decision points, 20-mV bidirectional step, 1-A feedback target with
100-mA deadband, 120-mV cumulative movement ceiling, 1.2-A physical IBUS stop,
single-primary topology, 4.30-V VBAT stop, 10--45-degree-C battery window and
all fail-closed cleanup rules remain unchanged. The first gate is a 240-second
run followed by explicit cancellation. A natural 15-minute run is allowed only
after that cancellation and rollback gate passes.

The C8 natural duration gate passed directly. The intended 1.25-A policy limit
was active, all six upward requests completed the exact 9010-to-9130-mV
movement, and all 1800 physical samples completed without an ADC range recheck.
Observed IBUS was 315--553.125 mA and die temperature was 34--37.5 degrees C.
Policy completion, pump/profile cleanup and fixed-5-V restoration all returned
zero.

## Stage 6C9 — conservative 1.5-A source-current request

C9 raises only the policy-owned PPS current request from 1.25 to 1.5 A. The
six feedback decisions, 20-mV step, 1-A target/deadband, 1.2-A physical IBUS
stop, APDO/9.4-V bounds, single-primary topology and every thermal/VBAT/cleanup
rule remain unchanged. Existing manual `*_1a` endpoints still use 1 A.

To shorten iteration, the first gate runs about 210 seconds: enough to complete
the sixth decision at roughly 180 seconds and observe it for about 30 seconds,
then explicitly cancel. A 15-minute run is deferred until a current request
brings IBUS near the existing 0.9--1.1-A feedback deadband.

The C9 natural duration gate passed directly. The intended 1.5-A policy limit
was active, all six upward requests completed the exact 9010-to-9130-mV
movement, and all 1800 physical samples completed without an ADC range recheck.
Observed IBUS was 603.75--836.25 mA and die temperature was 37--40 degrees C.
Policy completion and all rollback operations returned zero. The ending gauge
voltage was about 4.283 V, so another current gate requires a discharged battery
to avoid immediately reaching the unchanged 4.30-V stop.

## Stage 6C10 — 1.75-A target-window probe

C10 raises only the policy-owned PPS current request from 1.5 to 1.75 A. C9's
836.25-mA observed peak was still below the 0.9-A lower decision threshold, so
this next 250-mA request increment probes whether the established six-step
feedback reaches its 0.9--1.1-A hold window.

All voltage feedback, APDO/9.4-V bounds, the 1.2-A physical IBUS stop,
single-primary topology, 4.30-V VBAT stop, temperature window and cleanup rules
remain unchanged. Start only with gauge VBAT at or below about 4.20 V. Run about
210 seconds, then explicitly cancel; do not perform another natural 15-minute
run at this boundary.

The C10 cancellation gate passed after 444 valid physical samples. All six
decisions held the 8810-mV source request without a write, measured IBUS stayed
inside 939.375--1095 mA, die temperature stayed within 36--38.5 degrees C, and
all ADC-recheck and cleanup counters remained zero. This establishes 1.75 A as
the bounded single-primary source-current request; it must not be raised again.
One final natural 15-minute gate remains, using the same image and a starting
gauge voltage at or below about 4.00 V to avoid truncation by the 4.30-V stop.

The attempted accelerated detach closeout did not pass. After source removal,
samples 258 and 516 still showed the primary pump enabled and switching at only
22.5-mA IBUS. Residual VAC/VBUS remained above the physical voltage floor, so
the run waited for eventual hardware protection at sample 667 instead of
actively stopping. Cleanup and final off-state were correct, but C10 therefore
qualifies only the 1.75-A target window; detach handling remains an open gate.

## Stage 6C11 — input-loss guard and nonblocking reporting

C11 retains the C10 1.75-A source request, six feedback decisions,
0.9--1.1-A target window, 1.2-A physical stop and all rollback limits. The
policy now directly reads primary IBUS every 2 seconds. Three consecutive
readings below 500 mA stop the policy with `reason=input-loss` and use the
existing atomic cancellation path, so a stale qcom `USB_ONLINE` value cannot
leave the pump enabled indefinitely. The threshold remains below C10's
939.375-mA observed minimum and is debounced for roughly 6 seconds.

C11 also adds an atomic physical-run flag and changes `pulse_result` plus
`pulse_diagnostics` to use a nonblocking mutex acquisition. While the physical
run owns the mutex, reads immediately report `running=1 busy=1`.

The first gate is short: verify both active reads, remove source power without
writing the user-stop endpoint, and require automatic cancellation in about
6--8 seconds. The physical result must be `-ECANCELED`, cleanup must complete
and both pumps must be off. No 15-minute run is required.

Hardware passed this gate. The operator confirmed automatic pump shutdown a
few seconds after source removal and correct final policy, cancellation and
primary/secondary off-state outputs. The stale-`USB_ONLINE` detach gap exposed
by C10 is therefore closed.

## Stage 6C12 — self-contained primary startup

C12 removes the remaining manual `apply_init` prerequisite from policy startup.
When the controlled primary-pump target/preparation path first runs after boot,
it applies the same hardware-validated fail-closed profile before preparing the
pump. A profile write or readback failure still aborts startup before source
elevation or pump enable.

No electrical or lifecycle limit changes from C11. The gate is a fresh-boot
policy start without writing `apply_init`, followed by an explicit stop after
about 20 seconds and full off-state verification. This isolates startup
ownership before any later cable-insertion auto-start work.

Hardware passed this gate. Without `apply_init`, the policy entered active
operation, held primary IBUS in the validated target window and stopped cleanly
on the user request. The final policy was complete with zero cleanup/restore
errors, the physical run was cancelled cleanly after 60 samples, and all pump
outputs were reported correct and off.

## Stage 6C13 — attach-triggered auto-start

C13 adds an explicit DT opt-in that polls source state every 5 seconds and
automatically starts the qualified C12 policy once per physical attachment.
Startup requires primary-pump presence and at least 4-V pump-side VBUS,
Qualcomm USB online state, VBAT no higher than 4.20 V and the existing
10--45-degree-C battery-temperature window. A connection receives at most
three startup attempts. Reaching the active state disarms automatic startup;
a user stop also keeps it disarmed until a real detach is observed.

No electrical limit changes from C12. The short gate boots unplugged, confirms
the idle/armed state, plugs a validated PPS source without any sysfs setup or
start write, verifies automatic active operation, then explicitly stops and
requires no same-attachment restart plus complete pump/source rollback.

Hardware passed the complete lifecycle. The first attachment started
automatically; physical removal triggered the three-check input-loss guard,
stopped the pump, cleaned up without error and rearmed exactly once. A second
attachment started automatically and reached roughly 1.04--1.06-A primary
IBUS. User stop then completed cleanly with the pump off, `armed:0`, `starts:2`
and no restart while that source remained attached.

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
