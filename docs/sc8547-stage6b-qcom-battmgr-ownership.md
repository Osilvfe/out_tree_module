# Stage 6B Qualcomm battmgr transaction ownership

This document records an implementation constraint for the future Stage 6B
source-contract bridge. It is architecture documentation only; Stage 6B remains
unimplemented.

## Conclusion

If Caihong ultimately needs an extended Qualcomm battery-manager operation for
PPS contract control, it must share the existing `qcom_battmgr` transaction
serialization. A second independent PMIC-Glink client using the same BATTMGR
owner is not an acceptable design.

## Why

Linux PMIC-Glink routes an incoming packet to every registered client whose
client ID matches the packet owner. The existing `qcom_battmgr` driver already
registers a BATTMGR client and uses one driver-owned completion/error state for
its request/response transactions.

A second same-owner client would therefore receive the same responses as the
existing driver, while the existing driver would also see the second client's
responses. Because the existing transaction completion is not keyed by a
separate transaction ID, concurrent requests could be completed by the wrong
response.

The charging design must not depend on all other battery-manager traffic being
idle during a PPS operation.

## Required architecture

There must be one Linux owner for BATTMGR request/response serialization.

Preferred layering:

```text
qcom_battmgr
  - existing PMIC-Glink BATTMGR client
  - existing request lock/completion/service state
  - future narrowly scoped source-contract helpers
                |
                v
Stage-6 policy layer
                |
                v
sc8547_dual
                |
                v
SC8547 physical pair
```

The SC8547 drivers must not contain BATTMGR wire-format handling.

## Repository consequence

A future Stage-6B implementation may require a kernel-tree patch to
`drivers/power/supply/qcom_battmgr.c` in addition to the out-of-tree charging
modules. If so, that patch series must be listed explicitly as a Stage-6B test
prerequisite rather than being hidden inside an SC8547 module change.

## API boundary

A future policy layer should receive electrical operations such as source
capability discovery, one bounded PPS request and an explicit return-to-basic
operation. It should not receive a generic raw-property write primitive.

The common transaction owner must remain responsible for:

- request serialization;
- service-up/down state;
- completion/timeout handling;
- firmware error propagation;
- detach/reset handling.

## Remaining blocker

The exact Caihong operation that deterministically returns an active PPS source
contract to the normal/basic charging state still needs to be established.
Stage 6B must not gain a write-capable implementation until both request and
fallback semantics are documented.

## Future first hardware test

The first source-bridge hardware test remains independent of charge-pump start:

```text
both SC8547 pumps off
→ observe source capabilities/state
→ one conservative verified source-contract operation
→ verify completion and observed VBUS
→ explicitly return to basic/normal source state
→ verify ordinary PMIC charging recovery
```

Only after this source-only gate passes may a later policy stage combine source
control with charge-pump operation.