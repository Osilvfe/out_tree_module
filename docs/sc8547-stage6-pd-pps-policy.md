# SC8547 Stage 6 USB-PD/PPS policy boundaries

Stage 6 connects the already-separated charge-pump layers to USB source-contract
state. This stage must be developed more conservatively than the earlier local
CP controls because changing a USB-PD/PPS contract changes the electrical input
seen by the whole charging path, not just one SC8547 register.

This document is the design/test contract for Stage 6. Any later source commit
that can request a source voltage/current must update this document and
`sc8547-commit-test-matrix.md` before it is considered testable.

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
proved.

## Mainline Linux v7.2 evidence

`drivers/power/supply/qcom_battmgr.c` exposes the Qualcomm charger firmware as a
USB power supply named `qcom-battmgr-usb` on the SM8350/SM8550-style path.
The USB power supply reports:

- `POWER_SUPPLY_PROP_ONLINE`
- `POWER_SUPPLY_PROP_VOLTAGE_NOW`
- `POWER_SUPPLY_PROP_VOLTAGE_MAX`
- `POWER_SUPPLY_PROP_CURRENT_NOW`
- `POWER_SUPPLY_PROP_CURRENT_MAX`
- `POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT`
- `POWER_SUPPLY_PROP_USB_TYPE`

Its supported USB types include normal PD and `POWER_SUPPLY_USB_TYPE_PD_PPS`.
Therefore current mainline code can observe that charger firmware considers the
input to be a PD/PPS source and can report voltage/current-related values.

Important limitation: the USB `power_supply_desc` has `get_property` but no
USB `set_property` callback. Mainline Linux v7.2 therefore does **not** provide
an established public power-supply interface for this port to request a new
PD/PPS contract.

The Glink protocol constants in the same driver already include:

```text
BATTMGR_USB_PROPERTY_GET = 0x32
BATTMGR_USB_PROPERTY_SET = 0x33
```

but the existence of the SET opcode alone is not permission to invent property
IDs or their semantics in an out-of-tree policy driver.

## OnePlus downstream evidence: fixed PD and PPS are different operations

The Caihong downstream charging stack confirms that fixed PD and PPS are not one
interchangeable "set VBUS" operation.

### Fixed PDO path

`OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG` is forwarded through the virtual buck layer
to the platform charger implementation. The SM8350/QCOM implementation:

- accepts a 32-bit PDO;
- accepts only `PD_SRC_PDO_TYPE_FIXED`;
- decodes the fixed PDO voltage;
- permits only 5 V, 9 V and 12 V in that function;
- writes a downstream `BATT_SET_PDO`/`OPLUS_SET_PDO` property containing the
  requested voltage in mV;
- rejects battery, variable and augmented PDOs.

Therefore **BUCK_SET_PD_CONFIG is fixed-PD selection, not PPS control**.

### PPS path

PPS uses another function (`OPLUS_IC_FUNC_PPS_PDO_SET`) and two separate charger
firmware properties:

```text
USB_SET_PPS_VOLT
USB_SET_PPS_CURR
```

The downstream function writes requested VBUS in mV and requested IBUS in mA
through the USB-property SET path. The downstream header identifies the charger
firmware USB SET opcode as `BC_USB_STATUS_SET = 0x33`.

This distinction is a Stage-6 invariant. Future mainline-style work must not map
fixed PDO selection and PPS APDO requests onto one unverified generic property.

## What is still unverified

Before implementing any source-contract write bridge we still need to establish:

1. the exact numeric downstream property IDs used by the relevant Caihong/QCOM
   charger firmware for fixed-PDO and PPS voltage/current requests;
2. whether the public/mainline Qualcomm firmware ABI uses the same extended
   property numbers, or whether those IDs are Oplus additions;
3. whether charger firmware requires sequencing, authentication, adapter
   capability discovery or another enable step before PPS SET requests;
4. how success is acknowledged and how quickly `VOLTAGE_NOW`/USB type reflects
   the new contract;
5. the correct way to fall back to a known-safe 5 V/basic-charging state when a
   request fails or the charger firmware service resets;
6. whether a future upstreamable API should extend `qcom_battmgr`, expose an
   internal Qualcomm request API, or use another USB Type-C/PD subsystem.

Until these points are resolved, **Stage 6 contains no source-contract write**.

## Stage 6A — read-only source-contract diagnostics

Stage 6A is the next implementation step. It is intentionally observational.
It adds a separate diagnostic/policy platform driver above `sc8547_dual` and
reads the existing Qualcomm USB power supply.

### Proposed development-only DT

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

Planned module:

```text
sc8547_policy_diag.ko
```

The policy diagnostic module must consume a **read-only structured API** from
`sc8547_dual.ko`; it must not rediscover primary/secondary SC8547 devices or
read their registers directly.

Planned read-only attributes under a `sc8547_policy/` group:

- `usb_supply`
- `source_state`
- `combined_state`

`source_state` should report raw observable source information such as:

```text
online
usb_type (numeric and a local text label)
voltage_now_uv
voltage_max_uv
current_now_ua
current_max_ua
input_current_limit_ua
pps_detected
```

`combined_state` correlates that snapshot with the dual-CP snapshot:

```text
primary/secondary initialized + authorized
enabled + switching + mode + fault
primary/secondary VBUS/VBAT/IBUS
aggregate CP IBUS
source USB type/VBUS/current values
```

These values are diagnostics, not policy decisions. In particular:

- `VOLTAGE_MAX` is not automatically interpreted as a requested PPS voltage;
- `CURRENT_MAX` is not automatically interpreted as permission to draw it;
- `INPUT_CURRENT_LIMIT` is not automatically split between the two CPs;
- observing `PD_PPS` does not prove that a write API is available;
- aggregate SC8547 IBUS is not battery charge current.

### Stage-6A prohibition

Stage 6A must contain none of the following:

```text
power_supply_set_property()
pmic_glink_write()
BC_USB_STATUS_SET / BATTMGR_USB_PROPERTY_SET
USB_SET_PPS_VOLT / USB_SET_PPS_CURR
fixed PDO requests
automatic CP enable
```

If any of these appears, the change is no longer Stage 6A and requires a new
write-capable design/test gate.

## Stage-6A test sequence

Stage 6A can be tested with both charge pumps disabled. It does not require
Stage-5B dual switching to have passed, although the underlying Stage-5A pair
telemetry must already be trustworthy.

### Test 6A.0 — no source

1. Boot with both SC8547s and the virtual coordinator present.
2. Keep both CPs disabled.
3. Load the policy diagnostic module.
4. Capture `source_state` and `combined_state` with USB unplugged.
5. Confirm the module neither creates writable source controls nor changes CP
   register snapshots.

Expected: offline/unknown-or-idle source state, CPs off, no new fault caused by
loading the diagnostic layer.

### Test 6A.1 — ordinary 5 V source

1. Attach a known ordinary 5 V source.
2. Capture the Qualcomm USB power-supply sysfs values directly.
3. Capture `source_state` and `combined_state`.
4. Compare reported source VBUS with primary/secondary SC8547 VBUS ADC values.
5. Confirm CPs remain off unless separately and explicitly controlled.

This test establishes unit/scaling and observation consistency before PD is
introduced.

### Test 6A.2 — fixed-PD source

With a known PD adapter/cable, allow the existing Qualcomm firmware/mainline
stack to reach whatever fixed contract it normally selects; Stage 6A itself
must not request one.

Capture:

- USB type;
- voltage/current properties;
- both SC8547 VBUS ADCs;
- CP enable/switching state.

Do not proceed to source writes merely because `POWER_SUPPLY_USB_TYPE_PD` is
reported.

### Test 6A.3 — PPS-capable source

With a PPS-capable adapter/cable, observe whether Qualcomm firmware reports
`POWER_SUPPLY_USB_TYPE_PD_PPS` and record all source properties while CPs remain
off.

This is the critical evidence set for later bridge design. We need actual
Caihong values rather than assuming the SM8650 firmware behaves like another
Qualcomm device.

## Stage 6B — future source-request bridge, not implemented yet

Stage 6B will be a separate write-capable stage only after the firmware ABI is
confirmed.

It must begin with the charge pumps disabled and first prove contract control
without CP load. A likely validation sequence is:

1. observe stable 5 V/basic state;
2. issue one deliberately conservative, firmware-supported request through the
   verified Qualcomm interface;
3. require positive completion/ack;
4. verify resulting USB type and VBUS using both Qualcomm and SC8547 telemetry;
5. return to 5 V/basic state;
6. repeat while CPs remain off;
7. test firmware/service-reset and failed-request fallback;
8. only after all of this can a CP policy consume the bridge.

A source bridge that cannot demonstrably return to a safe/basic state must not
be connected to automatic CP start.

## Stage 6C — future CP/source policy and ramp

Only after Stage 5B and Stage 6B are independently hardware-validated should an
automatic policy be considered.

The first policy must be deliberately limited:

- one already-validated CP ratio;
- conservative source current;
- source request confirmed **before** CP enable;
- gradual voltage/current changes instead of jumping to final power;
- continuous observation of USB state, both CP faults and VBUS/VBAT/IBUS;
- immediate reverse-order CP stop on fault/input collapse;
- source fallback after CP stop;
- no VOOC/SuperVOOC/UFCS mixed into the generic PD/PPS state machine.

## Merge discipline for Stage 6

The intended commit pattern is:

```text
D  protocol evidence + electrical/test boundary
F  read-only virtual/policy API
F  read-only Stage-6A diagnostic module
F  build integration
C  focused Linux-v7.2 CI integration
D  exact commit/test matrix + roadmap status
```

Any later SET/write stage starts another documented sequence; it is not folded
silently into Stage 6A.

Every Stage-6 commit must be entered into `sc8547-commit-test-matrix.md` with:
meaning, write boundary, prerequisites, first meaningful test and promotion
status.