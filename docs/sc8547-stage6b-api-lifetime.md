# Stage 6B qcom_battmgr API identity and lifetime contract

This document refines the future Stage-6B kernel API boundary. It is design only:
no PMIC-Glink SET operation is implemented here.

The purpose is to prevent a future out-of-tree source test consumer from gaining
access to raw BATTMGR messages or accidentally treating an arbitrary
`struct power_supply *` as a qcom_battmgr instance.

## Required layering

The future source-control path is:

```text
6B-U0 source-only test consumer
        |
        | narrow electrical API
        v
qcom_battmgr Oplus extension
        |
        | sole BATTMGR request/ACK owner
        v
PMIC-Glink charger firmware
```

The SC8547 drivers remain outside this wire protocol.

## Do not export raw transaction primitives

The kernel-tree patch must not export any interface shaped like:

```text
qcom_battmgr_write(opcode, property, value)
```

or:

```text
qcom_battmgr_send_raw(...)
```

Those APIs would defeat the explicit firmware-ABI gate and expose the numeric
property collision to arbitrary callers.

Only named electrical operations may eventually be exported, for example:

```text
qcom_battmgr_oplus_fixed_5v(...)
qcom_battmgr_oplus_pps_request(..., voltage_uv, current_ua)
```

Names are not frozen. Semantics are.

## Why an arbitrary power_supply pointer is not sufficient identity

Linux exports `power_supply_get_drvdata()`, but the returned pointer is owned by
the individual power-supply driver. Calling it on an arbitrary power supply and
blindly casting the result to `struct qcom_battmgr *` would be a type-confusion
bug.

A future helper accepting `struct power_supply *` must first establish that the
object is the USB power supply registered by this qcom_battmgr implementation
before dereferencing qcom_battmgr-private state.

The validation belongs **inside qcom_battmgr.c**, where the driver knows its
private descriptors/instances. OOT code must not reproduce that test.

## Preferred public handle model

The simplest first development API is still allowed to use a referenced
`struct power_supply *`, because the Stage-6A layer already resolves
`qcom-battmgr-usb` through the power-supply class.

Required caller contract:

```text
power_supply_get_by_name() / equivalent reference acquisition
        |
        v
call narrow qcom_battmgr helper
        |
        v
power_supply_put()
```

Required callee validation, conceptually:

```text
1. reject NULL
2. establish that this psy is a qcom_battmgr USB supply
3. obtain the owning qcom_battmgr private object
4. require battmgr->usb_psy == supplied psy
5. require explicit Oplus firmware-ABI gate
6. only then inspect service/source state or transact
```

The exact implementation of step 2 should use driver-owned identity available
inside `qcom_battmgr.c`; it must not rely only on the user-visible power-supply
name string.

## Power-supply name is discovery, not authorization

`qcom-battmgr-usb` is useful for the development DT/test consumer to locate the
source telemetry object. It must not be treated as proof that private Oplus
property IDs are valid.

Authorization remains:

```text
correct qcom_battmgr-owned USB psy
AND
explicit Oplus firmware-ABI gate
AND
service/source preconditions
AND
operation-specific bounds
```

A renamed supply or another driver reusing a similar name must not acquire the
private source-control ABI.

## Reference lifetime

6B-U0 must hold a power-supply reference while invoking the exported helper. It
must not cache a raw pointer across remove/reprobe without a reference.

The qcom_battmgr helper owns no caller reference after it returns.

For a synchronous first implementation this keeps lifetime simple:

```text
get reference
  -> synchronous helper
  -> result/timeout
put reference
```

Do not introduce asynchronous PPS request completion into the exported API in
K2/K3. qcom_battmgr already has synchronous request/ACK behavior and Stage-6B
needs deterministic lab semantics more than throughput.

## Module/built-in linkage

A future kernel-tree API intended for the OOT Stage-6B consumer must be declared
in a small public kernel header and exported with GPL-only symbol visibility.

Conceptually:

```text
include/linux/power/qcom_battmgr.h
```

with only opaque/public types such as `struct power_supply` and integer electrical
units. The header must not expose `struct qcom_battmgr`, expected opcode/property
state, or PMIC-Glink message layouts.

This permits:

```text
qcom_battmgr built-in + sc8547/source consumer module
```

and, if configuration permits:

```text
qcom_battmgr module + consumer module
```

without making the private transaction object part of a module ABI.

The exact header location can be adjusted during K0 review; the boundary is the
important part.

## Unit contract

Any exported electrical API uses normal Linux units:

```text
voltage: microvolts
current: microamps
```

Conversion to Oplus downstream firmware units happens only inside qcom_battmgr:

```text
uV -> mV before property 34 / fixed-PDO property 25
uA -> mA before property 35
```

The helper must reject values that are not exactly representable or outside the
validated/request-authorized range rather than silently truncating a surprising
value.

## Capability ownership

K2/K3 should not make qcom_battmgr responsible for parsing the whole Linux
USB-PD class unless necessary.

For the first lab bridge, a clean division is:

```text
Stage-6A / U0:
  observe standard UCSI/USB-PD partner APDO capability
  choose a request inside an explicit lab window

qcom_battmgr K3:
  verify its own source state/gate
  enforce hard API bounds
  serialize exact firmware request
  validate firmware response
```

The caller must never be able to bypass qcom_battmgr's hard maximum bounds just
because userspace/DT claims a larger APDO.

Later policy integration may introduce a richer capability provider, but K3 does
not need to solve all PD policy architecture.

## Operation state after errors

The synchronous helper result represents **firmware transaction success**, not
proof of final electrical VBUS.

After K2/K3 returns success, U0 must independently verify observed source VBUS
using:

```text
qcom-battmgr USB telemetry
primary SC8547 VBUS ADC
secondary SC8547 VBUS ADC
```

If the request times out, the charger service resets, or the firmware reports an
error, U0 must treat the contract as unknown and keep both CPs off.

For a partial K3 failure after voltage SET succeeds but current SET fails, K3
must attempt K2 fixed-5V fallback before returning. The returned error must still
report the original PPS operation as failed even if fallback succeeds.

## K0/K1 implications

This document does not relax the implementation gate. When hardware evidence
unblocks source patches:

### K0

May add:

```text
- explicit Oplus firmware-ABI gate
- public narrow header declarations/stubs as appropriate
- private identity/lifetime helpers
- private pending-opcode/property fields
```

K0 must expose no callable electrical SET operation.

### K1

May add validated SET-response parsing and pending-response correlation.
K1 still exposes no source request/fallback caller.

Therefore K0 and K1 remain electrically inert checkpoints even though they
prepare the public/internal API structure.

### K2/K3

Only K2 first exposes a real source write (fixed 5 V). K3 later exposes bounded
PPS elevation.

## Future test implications

K0 test:

```text
boot with firmware gate absent
boot with firmware gate present
compare normal battmgr USB/battery behavior
no source request command exists
```

K1 test:

```text
same normal battmgr regression
no source request command exists
verify no new unknown-response warnings in ordinary GET traffic
```

K2/K3 testing remains exactly as defined in
`sc8547-stage6b-qcom-battmgr-patch-design.md`: CPs off, fallback first, PPS only
after fallback has hardware evidence.

## Implementation gate remains closed

Do not turn this API design into K0-K3 source commits until the real Caihong
Stage-6A captures and runtime/compiled-DT firmware-namespace evidence required by
the Stage-6B documents are available.
