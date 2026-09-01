# SC8547 stage-by-stage hardware test plan

This is the **operator-facing test and cherry-pick guide** for the OnePlus Pad
Pro (`caihong`) SC8547 charging work.

Use this file when constructing real-hardware test branches. Do **not** infer a
test sequence from the linear history of `sc8547-next`: development commits were
written ahead of hardware availability and therefore git-log order is not the
same thing as hardware-validation order.

For the meaning of every individual development commit, including CI,
documentation and temporary engineering commits, see:

```text
docs/sc8547-commit-test-matrix.md
```

For the long-term development roadmap, see:

```text
docs/sc8547-next-roadmap.md
```

## Rules used by every stage

1. Start from a branch whose previous stage has **passed on the same tablet**.
2. Cherry-pick only the functional commits listed for the stage being tested,
   plus explicitly desired documentation/CI commits.
3. Never cherry-pick the one-shot refactor machinery (`eaa26b8`, `63caf142`,
   `7aa2478`, `f5bc4a9`) into a clean test branch.
4. Build with the exact Caihong kernel/config before flashing. Focused torvalds
   v7.2 CI is only a compile gate, not a hardware-validation substitute.
5. Before any write-capable stage, save the previous stage's known-good logs.
6. On an unexplained I2C error, new blocking fault, implausible ADC value,
   unexpected CP switching or loss of normal PMIC charging: **stop at the
   current stage**. Do not proceed to the next one.
7. A stage is considered passed only after its explicit acceptance criteria are
   met. “It booted” is not a pass for a charging stage.
8. Keep primary and secondary results separate until Stage 5B.
9. Stage 6A is read-only and may be tested after Stage 5A without Stage 5B.
10. No Stage 6B source-contract write exists yet.

## Base checkpoint — Stage 0

### Branch base

```text
main @ 159d2e5  Update SC8547 telemetry test guide
```

### Meaning

Baseline physical SC8547 telemetry driver. The only intentional automatic
hardware write is ADC enable. It must not start the charge pump or rewrite the
experimental protection profile.

### Functional commits added for this stage

None; this is the base.

### Test flow

For **each** physical `0x6f` device:

```sh
cd /sys/bus/i2c/devices/<bus>-006f

cat sc8547/device_id
cat sc8547/role
cat sc8547/charge_enabled
cat sc8547/charge_mode
cat sc8547/switching
cat sc8547/adapter_present
cat sc8547/battery_present
cat sc8547/vbus_uv
cat sc8547/ibus_ua
cat sc8547/vbat_uv
cat sc8547/vout_uv
cat sc8547/vac_uv
cat sc8547/tdie_mc
cat sc8547/status_regs
cat sc8547/faults
```

Run once unplugged and once with a known ordinary 5 V source.

### Pass criteria

- both expected physical devices probe reliably;
- ADC values are plausible and change sensibly between unplugged/5-V states;
- CP enable/switching remains off unless some unrelated firmware path already
  controls it, which must be investigated before continuing;
- no new blocking fault appears merely from loading the module;
- normal `pmic-glink` / basic charging remains functional.

### If it passes

Proceed to Stage 1. Save all output as the Stage-0 reference capture.

---

# Stage 1 — silicon variant model and raw register snapshot

## Required previous stage

Stage 0 passed.

## Functional commits to add

```text
60c230c  Stage-1 variant model / register snapshot
```

## Optional documentation commits

```text
d425680  initial staged development roadmap
ab9c7b0  branch/test workflow policy
```

## Meaning of this stage

Adds silicon-family identification, runtime-ID checks, masked common helpers and
read-only register snapshots. It does **not** add a new charge-pump/protection
write path.

## DT changes

No experimental write opt-in is required.

Make sure the physical nodes carry correct roles:

```dts
southchip,role = "primary";
```

or:

```dts
southchip,role = "secondary";
```

## Test flow

For primary and secondary separately:

```sh
cd /sys/bus/i2c/devices/<bus>-006f
cat sc8547/device_id
cat sc8547/variant
cat sc8547/role
cat sc8547/register_dump
cat sc8547/status_regs
cat sc8547/faults
```

Run:

1. unplugged;
2. ordinary 5 V attached;
3. ordinary 5 V removed again.

## Pass criteria

- primary ID/variant is plausible; SC8547A ID `0x67` is expected from the
  downstream primary path;
- secondary identity is recorded rather than guessed;
- register dumps are repeatable in stable electrical states;
- no unexpected CP enable/switching is introduced;
- Stage-0 ADC/basic-charging behavior remains intact.

## Evidence to save

```text
stage1-primary-unplugged.txt
stage1-primary-5v.txt
stage1-secondary-unplugged.txt
stage1-secondary-5v.txt
```

These dumps become the raw reference for Stage 2 and the pre-write reference for
Stage 3.

## If it passes

Proceed to Stage 2.

---

# Stage 2 — protection-state decode, still read-only

## Required previous stage

Stage 1 passed and raw dumps were saved.

## Functional commits to add

```text
bc553aa  read-only protection_state decode
f15fb5a  bitfield helper include required by the decode
```

## Recommended documentation commit

```text
25d4a3d  Stage-2 decode/formula ambiguity documentation
```

## Meaning of this stage

Adds a read-only view of protection registers and vendor-header formula decodes.
It does **not** program protection thresholds. Formula-derived values are not
hardware-validated limits.

## Test flow

For each pump:

```sh
cd /sys/bus/i2c/devices/<bus>-006f
cat sc8547/register_dump
cat sc8547/protection_state
cat sc8547/faults
```

Compare `protection_state` raw bytes with the same registers in `register_dump`.
Repeat unplugged and at normal 5 V.

## Required review points

Keep these facts separate:

```text
downstream project raw input
vendor-header formula decode
nearby vendor comment
actual hardware threshold (still unproven)
```

In particular, do not turn the `0x36` / `0x0b` project values into automatic
mV/mA policy based only on comments or formulas.

## Pass criteria

- raw bytes in `protection_state` agree with `register_dump`;
- decoding is internally consistent with the documented header formula;
- no new writes or CP activity appear;
- Stage-1 telemetry remains stable.

## If it passes

Stage 0-2 read-only validation is complete. Only then construct a Stage-3 write
test branch.

---

# Stage 3 — controlled reset/protection init, pump kept off

## Required previous stage

Stage 0-2 passed on the real tablet and an exact raw profile has been reviewed
for the **specific** physical pump being tested.

## Functional commits to add

```text
aa3c510  initial gated Stage-3 reset/init/watchdog
29e6c6c  role-safe raw profile + stricter fail-closed behavior
17d7037  Linux-v7.2 API/format compatibility
```

Do not test `aa3c510` alone; use the complete Stage-3 functional set above.

## Recommended documentation commits

```text
2173e10  initial Stage-3 design contract
d6509c6  role-safe Stage-3 documentation alignment
```

Detailed procedure:

```text
docs/sc8547-experimental-control.md
```

## Useful CI commit

```text
4a0df97  focused Linux-v7.2 W=1 charging build
```

## Meaning of this stage

First intentional protection/control writes. The stage can reset the chip,
program explicitly supplied raw protection bytes and configure watchdog state,
but successful initialization must leave the charge pump disabled.

## DT requirements

On the one pump currently under test:

```dts
southchip,allow-experimental-control;

southchip,experimental-reg00 = <0xXX>;
southchip,experimental-reg02 = <0xXX>;
southchip,experimental-reg04 = <0xXX>;
southchip,experimental-reg05 = <0xXX>;

/* optional only when justified for this exact path */
southchip,experimental-reg01 = <0xXX>;
southchip,experimental-reg0d = <0xXX>;
```

Do **not** enable Stage-4 or Stage-5B opt-ins yet.

## Test one pump at a time

Recommended order:

```text
primary Stage 3
→ reboot/verify
→ secondary Stage 3
```

Before init:

```sh
cd /sys/bus/i2c/devices/<bus>-006f
cat sc8547/register_dump
cat sc8547/protection_state
cat sc8547_experimental/profile_raw
cat sc8547_experimental/init_state
cat sc8547_experimental/watchdog_ms
```

Save the output, then:

```sh
echo 1 > sc8547_experimental/apply_init
cat sc8547_experimental/init_state
cat sc8547/register_dump
cat sc8547/protection_state
cat sc8547/faults
cat sc8547/charge_enabled
cat sc8547/switching
cat sc8547/vbus_uv
cat sc8547/vbat_uv
```

## Pass criteria for each pump

- `init_state=initialized`;
- `charge_enabled=0`;
- `switching=0`;
- every required raw register reads back exactly;
- optional registers that were omitted were not intentionally programmed;
- ADC values remain plausible;
- no unexplained OVP/OCP/UCP/thermal fault appears;
- normal PMIC/basic charging remains usable afterwards.

## Required repetition

Run at least:

1. init from a fresh boot;
2. reboot and repeat;
3. remove experimental DT property/profile and confirm the ordinary telemetry
   path still boots without exposing Stage-3 controls.

## If either pump fails

Do not move that pump to Stage 4. Save before/after dumps and stop at Stage 3.

## If both pass

Proceed to Stage 4, still testing pumps separately.

---

# Stage 4 — manual single-pump switching

## Required previous stage

Stage 3 passed on the same physical pump. The other pump must remain disabled.

## Functional commit to add

```text
8033a03  gated manual single-pump mode/enable control
```

## Documentation/status commits

```text
ffd115b  Stage-4 design and test contract
1f9ab5c  Stage-3/4 roadmap checkpoint
4727cb9  records Stage-4 focused-v7.2 compile success
```

Detailed test document:

```text
docs/sc8547-stage4-manual-control.md
```

## Meaning of this stage

Adds manual `2:1` / `bypass` mode selection and single-pump enable behind a
second explicit opt-in and explicit VBUS/VBAT authorization windows. It does not
negotiate USB-PD/PPS and does not know about the peer pump.

## Additional DT requirements

Keep all validated Stage-3 properties and add:

```dts
southchip,allow-experimental-cp-enable;
southchip,experimental-vbus-min-uv = <VBUS_MIN>;
southchip,experimental-vbus-max-uv = <VBUS_MAX>;
southchip,experimental-vbat-min-uv = <VBAT_MIN>;
southchip,experimental-vbat-max-uv = <VBAT_MAX>;
```

The window must describe the controlled test setup; it is not a charger target.

## Test order

```text
primary only
→ stop and verify
→ repeat primary
→ secondary only
→ stop and verify
→ repeat secondary
```

Never enable both pumps during Stage 4.

## Test flow for each pump

```sh
cd /sys/bus/i2c/devices/<bus>-006f

cat sc8547/register_dump
cat sc8547/faults
cat sc8547_experimental/profile_raw
cat sc8547_experimental/enable_window

echo 1 > sc8547_experimental/apply_init
cat sc8547_experimental/init_state
cat sc8547/charge_enabled
cat sc8547/switching

echo '2:1' > sc8547_experimental/work_mode
cat sc8547_experimental/work_mode

cat sc8547/vbus_uv
cat sc8547/vbat_uv
cat sc8547/faults
cat sc8547/status_regs

echo 1 > sc8547_experimental/cp_enable
cat sc8547_experimental/cp_enable
cat sc8547/charge_enabled
cat sc8547/switching
cat sc8547/vbus_uv
cat sc8547/ibus_ua
cat sc8547/vbat_uv
cat sc8547/vout_uv
cat sc8547/faults

echo 0 > sc8547_experimental/cp_enable
cat sc8547/charge_enabled
cat sc8547/switching
```

Start with one ratio that is justified by the controlled source/battery state;
do not test multiple modes merely for coverage.

## Pass criteria for each pump

- Stage-3 init still passes;
- mode writes/readback are correct while disabled;
- invalid/incomplete authorization window refuses enable;
- successful enable produces `charge_enabled=1` and validated switching;
- ADC values remain plausible/in-window;
- no blocking fault appears;
- explicit disable reliably clears enable and switching stops;
- normal/basic charging recovers afterwards;
- the result is repeatable, not one accidental success.

## Hard gate

**Both primary and secondary must pass Stage 4 independently before Stage 5B
may be tested.**

Stage 5A, being read-only, may be tested earlier once the underlying telemetry
and shared-API regression checks pass.

---

# Stage 5A — read-only virtual two-pump pairing

## Required previous stage

For read-only testing: Stage 1-2 telemetry must be trusted. If using the current
shared physical API stack below, rerun the Stage-4 single-pump smoke test after
the API refactor before later proceeding to Stage 5B.

## Canonical functional commits for the current Stage-5A test package

```text
18581b0  add read-only sc8547_dual coordinator
ec05245  build sc8547_dual.ko
6986e8c  require distinct primary/secondary roles
60cb97e  add shared physical-driver API declaration
e854be9  keep shared API opaque
9b29a00  implement shared physical safety API; Stage-4 sysfs uses it
7caaee0  move dual telemetry onto shared physical state API
```

## Important excluded commits

Do not cherry-pick the one-shot machinery:

```text
eaa26b8
63caf142
7aa2478
f5bc4a9
```

Do not add Stage-5B write commit `25d0e27` merely to test Stage 5A.

## Recommended documentation/CI

```text
1801dae  virtual coordinator DT architecture
764f821  Stage-5A build-status checkpoint
3cbdb91  shared physical API ownership contract
edf0998  two-module focused CI (historical; current CI is later)
```

Detailed document:

```text
docs/sc8547-stage5-dual-coordinator.md
```

## Meaning of this stage

Creates a separate virtual pair layer. It resolves two physical SC8547 devices
by DT phandles and exposes paired telemetry/aggregate IBUS. Stage 5A adds no
new dual-pump enable operation.

The shared-API refactor (`9b29a00`) changes the code path used by existing Stage
4 sysfs, so **Stage-4 regression is a prerequisite before trusting Stage 5B**,
even though Stage-5A's own virtual controls are read-only.

## DT

Physical nodes must have exact roles. Add a separate coordinator:

```dts
sc8547_dual: charge-pump-coordinator {
    compatible = "southchip,sc8547-dual-experimental";
    southchip,primary = <&cp_primary>;
    southchip,secondary = <&cp_secondary>;
};
```

Do **not** add:

```dts
southchip,allow-experimental-dual-cp;
```

for Stage 5A.

## Regression step after `9b29a00`

Before pair testing, rerun the successful Stage-4 single-pump smoke sequence on
primary and secondary, one at a time. The behavior should be unchanged because
`9b29a00` is an implementation refactor, but that assumption must be verified
on hardware before Stage 5B.

## Pair test flow

With both pumps disabled:

```sh
modprobe sc8547_cp
modprobe sc8547_dual

cd /sys/bus/platform/devices/<coordinator>/sc8547_dual
cat peer
cat pair_state
cat aggregate_ibus_ua
cat last_result
```

Repeat unplugged and at ordinary 5 V.

Also verify that Stage-5B writable files are absent:

```sh
ls -l /sys/bus/platform/devices/<coordinator>/sc8547_dual
```

## Negative role test

In a separate DT test, deliberately give one referenced device the wrong role.
The virtual pair must be refused; no physical write may occur.

Restore the correct DT before continuing.

## Pass criteria

- coordinator identifies the intended two physical buses/devices;
- roles are exactly primary/secondary;
- per-pump snapshot matches direct physical telemetry;
- `aggregate_ibus_ua` equals primary + secondary reported IBUS;
- both pumps remain off;
- no Stage-5B writable controls are exposed;
- reading/loading the virtual driver does not change physical register dumps;
- Stage-4 regression after the shared-API refactor passed.

## If it passes

Two different next steps are allowed:

```text
Stage 5B  only if both pumps passed Stage 4
Stage 6A  read-only source diagnostics, even if Stage 5B has not been tested
```

---

# Stage 5B — explicit dual-pump laboratory start/stop

## Required previous stages

All of the following must be true:

- Stage 5A passed;
- primary Stage 4 passed repeatedly;
- secondary Stage 4 passed repeatedly;
- Stage-4 regression after `9b29a00` passed on both pumps.

## Functional commit to add

```text
25d0e27  gated dual-pump work_mode + dual_enable coordination
```

## Required test document

```text
0c92c3a  implemented Stage-5B control/rollback test contract
```

Status documentation:

```text
2fc76f1  records focused Linux-v7.2 Stage-5B build success
```

Detailed procedure:

```text
docs/sc8547-stage5-dual-coordinator.md
```

## Meaning of this stage

Adds the first dual-pump write path. It reuses the physical driver's validated
Stage-4 safety API. Start order is primary then secondary; stop/rollback order
is secondary then primary. It still does not request a USB-PD/PPS source
contract.

## Additional DT opt-in

On the virtual coordinator only:

```dts
southchip,allow-experimental-dual-cp;
```

Both physical devices keep their individually validated Stage-3/4 profiles and
windows.

## Safe refusal test first

Before a successful dual-start attempt, deliberately remove Stage-4
authorization from one pump in DT and verify:

```sh
echo 1 > sc8547_dual/dual_enable
```

is refused before either pump starts.

Do **not** manufacture electrical OVP/OCP/thermal faults to test rollback.

Restore the validated Stage-4 DT before the real dual test.

## First dual-start flow

Use the **same ratio and source conditions already proven in Stage 4**:

```sh
cd /sys/bus/platform/devices/<coordinator>/sc8547_dual

cat pair_state
cat last_result

echo '2:1' > work_mode
cat work_mode
cat pair_state

echo 1 > dual_enable
cat dual_enable
cat pair_state
cat last_result

# First bring-up: stop immediately after the snapshot.
echo 0 > dual_enable
cat dual_enable
cat pair_state
cat last_result
```

## Pass criteria

- both were initialized/authorized/off before start;
- requested modes match;
- primary reaches its validated switching state before secondary is enabled;
- successful pair state shows both enabled + switching + no blocking fault;
- VBUS/VBAT/IBUS remain plausible and within each physical Stage-4 window;
- explicit stop returns both pumps to off;
- secondary/final failure path leaves both pumps off;
- normal PMIC/basic charging recovers after stop.

## Initial Stage-5B scope

A Stage-5B pass proves only controlled dual switching. It does **not** prove:

- safe high-power operation;
- 6 A input capability;
- current-sharing policy;
- thermal policy;
- PD/PPS source negotiation;
- automatic fast charging.

Do not increase power merely because dual switching passed.

---

# Stage 6A — read-only USB source / CP correlation

## Required previous stage

Stage 5A passed. Stage 5B is **not required** because Stage 6A does not start
pumps or change the USB source contract.

## Functional commits to add on top of the current Stage-5A package

```text
e16d7c2  declare read-only virtual-pair state API
fc32392  implement/export sc8547_dual_get_state()
9ec6520  add sc8547_policy_diag read-only diagnostic layer
756acee  build sc8547_policy_diag.ko
```

## CI commit

```text
97892e6  focused v7.2 W=1 builds all three charging modules
```

## Documentation/evidence commits

```text
a48fa1f  Stage-6 architecture + 6A/6B/6C separation
f0a0e9d  downstream PPS session/start/ramp/rollback evidence
```

Detailed document:

```text
docs/sc8547-stage6-pd-pps-policy.md
```

## Meaning of this stage

Adds a third, read-only policy-diagnostic layer above `sc8547_dual`. It
correlates the existing Qualcomm USB power-supply view with both charge pumps.
It contains no `power_supply_set_property()`, no PMIC-Glink SET, no PPS request
and no automatic CP enable.

## DT

Keep Stage-5B opt-in **absent**.

Add the diagnostic node:

```dts
sc8547_policy_diag: charge-policy-diagnostic {
    compatible = "southchip,sc8547-policy-diagnostic";
    southchip,charge-pump = <&sc8547_dual>;
    southchip,usb-power-supply-name = "qcom-battmgr-usb";
};
```

## Stage-6A test sequence

### 6A.0 — unplugged

```sh
cd /sys/bus/platform/devices/<policy-diagnostic>/sc8547_policy
cat usb_supply
cat source_state
cat combined_state
```

Confirm both pumps are off and Stage-5B writable controls remain absent.

### 6A.1 — ordinary 5 V

Attach a known ordinary source. Capture direct Qualcomm values and diagnostics:

```sh
for f in online voltage_now voltage_max current_now current_max input_current_limit usb_type; do
    [ -e /sys/class/power_supply/qcom-battmgr-usb/$f ] && \
        printf '%s=' "$f" && cat /sys/class/power_supply/qcom-battmgr-usb/$f
done

cat sc8547_policy/source_state
cat sc8547_policy/combined_state
```

Compare source VBUS with both SC8547 VBUS ADC readings.

### 6A.2 — naturally selected fixed PD

Use a known PD adapter/cable, but let existing Qualcomm firmware/mainline choose
whatever contract it normally chooses. Stage 6A must not issue a request.

Capture the same data and keep both pumps off.

### 6A.3 — PPS-capable adapter observation

Use a known PPS-capable adapter/cable. Observe whether `usb_type` reports
`PD_PPS` naturally. Capture:

- adapter/cable identity;
- all direct `qcom-battmgr-usb` values;
- `source_state`;
- `combined_state`;
- both physical VBUS/IBUS/VBAT snapshots.

## Pass criteria

- diagnostic values agree with direct power-supply values/scales;
- 5-V source state is internally consistent before PD/PPS conclusions are made;
- CP state remains off throughout;
- loading/reading diagnostics never changes source contract;
- fixed-PD/PPS observations correlate plausibly with both physical VBUS ADCs;
- no Stage-5B controls become visible merely because Stage 6A exists.

## Meaning of a Stage-6A pass

It proves **observation only**. It provides the Caihong evidence required to
design Stage 6B. It is not permission to send downstream property 34/35 or any
other source-contract SET command.

---

# Stage 6B — source-contract bridge

## Current status

**Not implemented. Research/documentation only.**

There is intentionally no Stage-6B functional commit to cherry-pick yet.

## Known downstream evidence so far

The current evidence shows:

- fixed-PD PDO selection and PPS APDO requests are different operations;
- the non-SoCCP downstream USB property namespace has PPS voltage/current SET
  properties at IDs 34/35, with mV/mA arguments;
- downstream property SET uses charger-firmware ACK/error handling;
- downstream APDO/PDO capability read uses a separate PPS read-buffer opcode;
- downstream PPS startup first validates source/capability/charge conditions,
  chooses CP ratio, requests a conservative low-power PPS point, ramps source
  VBUS, and only then starts the charge pump;
- rollback stops CP load before leaving the PPS path;
- source-code DT analysis strongly suggests Caihong uses the non-SoCCP path, but
  final compiled/runtime DT must still confirm that before any write test;
- upstream Linux v7.2 UCSI can read partner PDO capabilities when the platform
  advertises `UCSI_CAP_PDO_DETAILS`; whether Qualcomm UCSI provides a suitable
  sink-contract/PPS request API remains under investigation.

## Required evidence before a Stage-6B write commit may exist

1. Capture Stage-6A 6A.0–6A.3 data from the real tablet.
2. Confirm the final runtime/compiled DT does not select an incompatible SoCCP
   property namespace.
3. Confirm whether Caihong UCSI exposes partner PDO/APDO details.
4. Determine whether a standard UCSI/USB-PD kernel interface can request the
   required sink contract; prefer that over private Oplus protocol copying.
5. If a Qualcomm/Oplus firmware bridge is still necessary, document exact
   opcode, property IDs, units, ACK/error semantics and service-reset behavior.
6. Define and implement an explicit **CP-off source-only test** first.
7. Define a deterministic return-to-basic/5-V path and verify it before any CP
   policy can consume the bridge.

## Planned first Stage-6B hardware test

When, and only when, a future Stage-6B implementation satisfies the above:

```text
both CPs OFF
→ observe stable basic/5-V state
→ read source capabilities
→ issue one conservative verified source request
→ require protocol/firmware success acknowledgement
→ verify resulting VBUS in Qualcomm telemetry + both SC8547 ADCs
→ return to basic/5-V state
→ verify normal PMIC charging recovery
```

No charge pump will be enabled in the first Stage-6B hardware test.

---

# Stage 6C — automatic source + CP policy

## Current status

Not implemented and not eligible for development until:

```text
Stage 5B hardware pass
AND
Stage 6B source-only hardware pass
```

The future first policy must keep one validated CP ratio, conservative current,
source contract before CP enable, bounded voltage/current ramp, continuous
fault/source observation, reverse-order CP stop and source fallback.

---

# Canonical stage progression summary

For a full write-capable hardware-validation campaign, the order is:

```text
Stage 0  baseline telemetry
  ↓ PASS
Stage 1  variant + raw snapshot
  ↓ PASS
Stage 2  protection decode, read-only
  ↓ PASS
Stage 3  controlled init, CP stays off
  ↓ PASS on primary + secondary
Stage 4  single-pump switching
  ↓ PASS independently on primary + secondary
Stage 5A virtual pair telemetry + shared-API regression
  ↓ PASS
Stage 5B dual-pump switching
  ↓ PASS
Stage 6B source-only contract control   [not implemented]
  ↓ PASS
Stage 6C automatic policy              [not implemented]
```

The independent read-only observation path is:

```text
Stage 5A PASS
  ↓
Stage 6A source/CP diagnostics
```

Stage 6A does not require or authorize Stage 5B.

# What to record after every stage

Create a directory named with the stage and test date. Save at minimum:

```text
kernel commit / cherry-pick list
DT commit / exact DTS diff
kernel config
module hashes
full dmesg from boot through test
relevant sysfs captures
adapter/cable/source description
pass/fail result
first unexplained anomaly, if failed
```

For every write-capable test also save:

```text
pre-write register snapshot
post-write register snapshot
fault/status snapshot immediately after operation
explicit shutdown/disable result
normal-charging recovery result
```

Do not erase a failed stage's evidence by immediately trying the next stage.
Fix/review the failed stage first.