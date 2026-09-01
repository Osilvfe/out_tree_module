# SC8547 Stage 6B PPS request and fallback contract

This document records the first complete downstream request/fallback chain that
is relevant to a future Caihong Stage-6B source-contract bridge.

It is **documentation only**. No USB source write is implemented by this
commit.

The operator-facing hardware sequence remains in:

```text
docs/sc8547-stage-test-plan.md
```

The source-interface/ownership research is in:

```text
docs/sc8547-stage6b-source-interface-research.md
docs/sc8547-stage6b-qcom-battmgr-ownership.md
```

## Main conclusion

The downstream PPS `EXIT` operation is **not** itself the electrical operation
that returns the adapter to 5 V.

The actual high-level normal-return sequence contains an explicit fixed-PDO
5-V request:

```text
stop/tear down CP use
        |
        v
PPS virtual IC EXIT
        |
        |  only marks the Oplus PPS virtual IC offline
        v
oplus_pps_switch_to_normal()
        |
        +--> FIXED_PDO_SET(5000 mV, 3000 mA)
        |
        +--> DPDM_SWITCH_TO_AP
        |
        v
release wired-suspend vote / resume normal charging policy
```

This gives Stage 6B a concrete candidate for deterministic rollback: **PPS
request must be paired with an explicit fixed-PDO 5-V fallback**.

## Evidence: PPS virtual IC EXIT does not change the source contract

The downstream `oplus_chg_adsp_pps_exit()` implementation only:

```text
checks ic_dev
sets ic_dev->online = false
triggers OPLUS_IC_VIRQ_OFFLINE
returns 0
```

It sends no PMIC-Glink property request and does not ask the source for a fixed
contract.

Therefore a future mainline bridge must not model `OPLUS_IC_FUNC_EXIT` as the
source-fallback primitive.

## Evidence: switch_to_normal explicitly requests fixed 5 V

Downstream `oplus_pps_switch_to_normal()` contains the comment:

```text
switch to 5v when switch to normal
```

and, while wired input is online, calls:

```text
OPLUS_IC_FUNC_FIXED_PDO_SET
voltage = 5000 mV
current = 3000 mA
```

before switching DPDM back to AP.

The 3000-mA argument is part of the Oplus function signature/call, but the
platform fixed-PDO implementation currently selects one of predefined fixed PDO
values by voltage. Do not interpret this call-site current argument as a new
permission to draw 3 A on an arbitrary adapter.

## Fixed-PDO platform implementation

The Oplus platform charger accepts only fixed 5/9/12-V requests in this path.
For 5 V it constructs/uses the downstream fixed-PDO representation and reaches
`oplus_chg_8350_set_pd_config()`.

On the non-SoCCP path that helper performs a normal battery-property SET:

```text
opcode      = BC_BATTERY_STATUS_SET = 0x31
property_id = BATT_SET_PDO
value       = requested fixed voltage in mV
```

For the fallback of interest:

```text
value = 5000
```

The transaction uses the same downstream battery-manager request/ACK machinery
as other charger properties.

## Exact Oplus non-SoCCP property number

The downstream battery-property enum has the common entries 0 through 23, then:

```text
24 = BATT_CHG_EN
25 = BATT_SET_PDO
26 = BATT_SET_QC
...
```

Therefore the current downstream Caihong candidate fallback operation is:

```text
owner       = BATTMGR / MSG_OWNER_BC
req/resp
opcode      = 0x31
property    = 25
value       = 5000   # mV
```

This is **not yet permission to send it from mainline Linux**.

## Critical ABI collision with upstream qcom_battmgr

Linux v7.2 `qcom_battmgr` uses the same common battery-property numbering
through property 23, but then defines a different extension namespace:

```text
24 = BATT_CHG_CTRL_EN
25 = BATT_CHG_CTRL_START_THR
26 = BATT_CHG_CTRL_END_THR
```

Oplus downstream instead uses:

```text
24 = BATT_CHG_EN
25 = BATT_SET_PDO
26 = BATT_SET_QC
```

Thus `property = 25` is **firmware-ABI dependent**.

A generic upstream-style helper such as:

```text
qcom_battmgr_write_property(25, 5000)
```

would be unacceptable: on a different Qualcomm firmware ABI the same numeric
property has another meaning.

Any future implementation must positively select the Caihong/Oplus charger
firmware extension before exposing `BATT_SET_PDO` semantics.

## Interaction with current upstream charge-control code

Linux v7.2 also supports charge-control thresholds on some battmgr variants,
but those writes use a dedicated charge-control opcode (`0x48`) rather than
blindly sending battery property 25/26 through the battery-property SET path.

That reduces accidental runtime overlap in the current driver, but it does not
remove the ABI-number collision. The numeric namespace must still be treated as
variant/firmware-specific.

## PPS request side

The current non-SoCCP downstream PPS request candidate remains:

```text
opcode 0x33 (USB property SET)
property 34 = USB_SET_PPS_VOLT, value in mV
property 35 = USB_SET_PPS_CURR, value in mA
```

Both writes go through the serialized battery-manager transaction path and must
receive successful firmware acknowledgement.

The high-level downstream startup uses a conservative initial PPS point before
charge-pump start:

```text
5500 mV @ 800 mA   Oplus-adapter path
5500 mV @ 1000 mA  third-party PPS path
```

This does **not** mean the first mainline Stage-6B test will automatically use
those values. The real Caihong Stage-6A capability capture must first prove that
the attached adapter advertises a matching PPS APDO and that the platform
firmware accepts the selected operation.

## Candidate reversible transaction for the first source-only experiment

Only after all Stage-6B prerequisites are met, the first write-capable test is
expected to have the shape:

```text
both SC8547 pumps OFF
        |
        v
verify source is online and PPS-capable
        |
        v
read/validate partner APDO capability
        |
        v
issue one conservative PPS request
  USB SET 0x33 / verified Oplus properties
        |
        v
require firmware ACK
        |
        v
verify actual VBUS through:
  - qcom-battmgr telemetry
  - primary SC8547 VBUS ADC
  - secondary SC8547 VBUS ADC
        |
        v
explicit fixed-PDO fallback
  BAT SET 0x31 / Oplus BATT_SET_PDO / 5000 mV
        |
        v
require firmware ACK
        |
        v
verify VBUS returned near 5 V
        |
        v
verify normal PMIC charging recovered
```

No CP enable belongs in this first experiment.

## Fail-closed requirements for the future bridge

A future source bridge must:

1. serialize through the existing qcom_battmgr transaction owner;
2. reject operation unless the selected platform is explicitly known to use
   the Oplus extension namespace;
3. reject PPS requests unless source type/capability checks pass;
4. propagate firmware negative return values;
5. propagate request timeout/service-down/detach errors;
6. never report PPS success until VBUS is independently observed in range;
7. expose an explicit 5-V fallback operation;
8. attempt fallback after any later policy failure once a PPS request has been
   accepted;
9. keep both charge pumps off during Stage-6B source-only validation.

## Still required before source code

Despite identifying both request and fallback candidates, the first
write-capable Stage-6B patch remains blocked until these items are completed:

- real Caihong Stage-6A source/APDO captures;
- final runtime/compiled-DT confirmation of the non-SoCCP path;
- a positive platform/firmware selector for the Oplus extension namespace;
- exact qcom_battmgr response handling for the extended SET operations;
- review of detach and PMIC-Glink service-reset behavior during an active
  source transaction;
- definition of the exact conservative first request and allowed voltage/current
  range from real adapter capabilities.

Finding `property 25 = BATT_SET_PDO` is evidence, not an implementation gate by
itself.