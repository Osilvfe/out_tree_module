# Stage 6B Qualcomm battmgr transaction ownership

This document records the transaction-ownership constraint for the future
Stage-6B source-contract bridge. It is architecture documentation only; no
source-contract write is implemented here.

## Current conclusion

If Caihong ultimately needs the Oplus Qualcomm battery-manager extension ABI for
exact PPS control, that support must live inside the existing Linux
`qcom_battmgr` transaction owner, or inside a deliberately refactored common
transaction core owned by it.

A second independent PMIC-Glink client using `PMIC_GLINK_OWNER_BATTMGR` is not
an acceptable implementation.

## Why a second BATTMGR client is unsafe

Linux PMIC-Glink delivers an incoming packet to every registered client whose
client ID matches the packet owner. The current `qcom_battmgr` driver already
registers one BATTMGR owner and maintains one request state:

```text
battmgr->lock
battmgr->ack
battmgr->error
```

`qcom_battmgr_request()` performs:

```text
reinit_completion(ack)
error = 0
pmic_glink_send()
wait_for_completion_timeout(..., 1 second)
return error
```

There is no independent transaction ID in the common property request. Existing
power-supply GET paths hold `battmgr->lock` around the request so only one
normal battmgr request is outstanding.

A second same-owner client would receive qcom_battmgr's responses and
qcom_battmgr would also receive that client's responses. A response from one
client could therefore complete the other driver's unkeyed completion. The
Stage-6 design must never depend on ordinary battery/USB property reads being
idle during PPS control.

## Existing request owner remains authoritative

Preferred layering:

```text
qcom_battmgr
  - sole BATTMGR PMIC-Glink transaction owner
  - existing mutex/completion/error/service state
  - future explicitly gated Oplus source-contract helpers
                |
                v
Stage-6B source-only test/policy consumer
                |
                v
Stage-6C automatic policy (later)
                |
                v
sc8547_dual
                |
                v
SC8547 physical pair
```

The SC8547 physical or virtual drivers must not contain BATTMGR wire-format
handling.

## SM8650 variant detail

Mainline SM8650 PMIC-Glink DT uses the compatible chain:

```text
qcom,sm8650-pmic-glink
qcom,sm8550-pmic-glink
qcom,pmic-glink
```

Linux v7.2 `qcom_battmgr` therefore matches its SM8550-family variant handling
on an SM8650 platform.

This **must not** be used as the Oplus-extension enable condition. Other
SM8550/SM8650 devices can use a different charger-firmware property namespace.
The Oplus property meanings must be selected by an explicit firmware-ABI gate,
not by SoC family alone.

## Where the explicit DT gate can live

The PMIC-Glink core creates the `power-supply` auxiliary device with the parent
PMIC-Glink OF node. Consequently the future `qcom_battmgr` extension can read an
explicit development-only property from the PMIC-Glink node without creating a
second device/client.

Conceptually:

```dts
&pmic_glink {
    oplus,caihong-battmgr-pps-extension;
};
```

The exact local property name is not frozen, but its semantics are:

- explicit firmware-ABI authorization;
- absent by default;
- never inferred merely from `sm8550`/`sm8650` compatible;
- no electrical action merely because the property exists.

## SET-response handling is a mandatory kernel change

Linux v7.2's SM8350/SM8550-style callback explicitly handles property GET
responses, but it does not currently provide validated generic handling for the
Oplus BAT/USB SET operations required here.

A future Stage-6B kernel patch must not rely on the callback's unknown-message
path merely reaching the shared completion. It must validate the SET response:

1. payload length;
2. response opcode;
3. returned property ID;
4. firmware result/return code;
5. expected pending opcode/property;
6. only then complete the request as successful.

Because the wire request lacks a transaction ID, the future extension should
track private expected state while the existing mutex serializes transactions:

```text
ext_pending
ext_expected_opcode
ext_expected_property
```

No raw opcode/property write API should be exported to SC8547 or userspace.

## Candidate Oplus operations now documented

The source-request and return-to-basic candidates are now documented in
`sc8547-stage6b-request-fallback.md`.

For the current non-SoCCP evidence:

```text
PPS voltage request:
  USB PROPERTY SET opcode 0x33
  Oplus property 34
  value in mV

PPS current request:
  USB PROPERTY SET opcode 0x33
  Oplus property 35
  value in mA

fixed-5V fallback:
  BAT PROPERTY SET opcode 0x31
  Oplus property 25 (BATT_SET_PDO)
  value 5000 mV
```

The fallback candidate is no longer the main unknown. The remaining blockers
are positive runtime/platform ABI confirmation, real APDO capability data and a
validated kernel-side SET-response implementation.

## Critical property-number collision

Oplus property 25 is not generic Qualcomm ABI. In Linux v7.2 the upstream
battery-property extension assigns another meaning to numeric property 25.
Therefore no future helper may expose:

```text
write_property(25, value)
```

to arbitrary callers.

The Oplus meaning exists only under the explicit Caihong firmware gate and must
use visibly namespaced constants.

## Service-down behavior

`qcom_battmgr_pdr_notify()` currently updates `service_up` when the charger
service goes up/down, but a service-down event does not itself complete an
already pending property request. A request can therefore time out if the
service disappears while it is waiting.

Stage 6B must treat this as an unknown contract state:

```text
service down / request timeout
→ never report source request success
→ keep/force both CPs off
→ after service returns, re-read source state/capabilities
→ do not assume the previous PPS contract survived
```

The first Stage-6B patch does not need to redesign all qcom_battmgr recovery,
but K0/K1 review and later source-only tests must preserve and document this
behavior.

## Source detach versus fallback failure

If the cable/source is detached, an explicit 5-V fallback may fail because
there is no source to renegotiate. Confirmed detach is different from a failed
request against an attached source.

The policy layer must distinguish:

```text
source detached:
  CPs off; source contract no longer relevant

source attached + fallback failed:
  CPs off; source state unknown; do not continue policy
```

## Kernel-tree consequence

Stage 6B is no longer expected to be implementable entirely in
`out_tree_module`.

If the Oplus ABI is confirmed on real Caihong hardware, the clean test package
will contain two repositories/commit sets:

```text
kernel tree:
  qcom_battmgr K0/K1/K2/K3 series

out_tree_module:
  source-only bounded test consumer (later 6B-U0)
```

Every kernel-tree commit must be listed explicitly in the Stage package index
with the same meaning/test-gate discipline used for the OOT commits.

## Planned kernel patch ladder

The current design is deliberately ordered:

```text
6B-K0  explicit firmware-ABI gate + private plumbing; no callable SET
6B-K1  validated BAT/USB SET response parsing; still no source request caller
6B-K2  fixed-5V fallback helper only
6B-K3  bounded exact PPS helper, using K2 on partial failure
6B-U0  separate source-only manual test consumer, CPs required off
```

Fallback is implemented and hardware-tested before PPS elevation.

## Current implementation gate

Do not create K0/K1/K2/K3 electrical write commits until all of these are
available from the real tablet:

1. Stage 6A F0-F3 captures;
2. runtime/compiled DT confirmation of the intended non-SoCCP/Oplus firmware
   namespace;
3. partner PPS APDO capability/range evidence, preferably through UCSI/USB-PD
   sysfs when available;
4. a conservative first request bounded by the real adapter capability;
5. an agreed kernel test branch where qcom_battmgr can be patched and reverted
   independently.

Design and compile planning may continue before those facts exist; electrical
SET code does not.

## Future first source hardware test

The first Stage-6B electrical test remains independent of charge-pump start:

```text
both SC8547 pumps OFF
→ observe normal source state
→ verify source capability and explicit ABI gate
→ test K2 fixed-5V request while CPs remain off
→ verify firmware result + observed ~5 V
→ only after K2 passes, test one bounded K3 PPS request
→ verify firmware result + VBUS in qcom-battmgr and both SC8547 ADCs
→ immediately call K2 fallback
→ verify ~5 V and ordinary PMIC charging recovery
```

Only after this source-only gate passes may Stage 6C combine source control with
charge-pump operation.
