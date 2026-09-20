# Stage 6B gated qcom_battmgr extension design

This document defines and records the **minimum kernel-tree change** used to
validate Caihong's Oplus battery-manager PPS extension ABI.  The local kernel
tree now contains narrowly gated, fixed-value development endpoints; it still
does not expose an arbitrary voltage/current control or start either charge
pump.

## Why a qcom_battmgr patch is likely required

Current evidence has established all of the following:

- exact PPS `(mV, mA)` requests are not exposed through Linux v7.2's normal
  UCSI/power-supply API;
- Oplus downstream uses BATTMGR owner requests for exact PPS voltage/current;
- PMIC-Glink routes all BATTMGR-owner responses to every same-owner client;
- therefore a second independent out-of-tree BATTMGR client would race the
  existing `qcom_battmgr` completion state;
- the safe place for an Oplus extension is the existing BATTMGR transaction
  owner, or a future shared transaction core derived from it.

The first implementation should be the smallest possible extension of
`drivers/power/supply/qcom_battmgr.c` rather than a new Glink client.

## Development-only platform gate

Oplus property IDs conflict numerically with the generic Qualcomm namespace, so
these semantics must never be enabled solely because the SoC is SM8550/SM8650.

The first development patch should require an explicit firmware-ABI opt-in on
the PMIC-Glink device, conceptually:

```dts
&pmic_glink {
    oplus,caihong-battmgr-pps-extension;
};
```

The exact local property name can still be refined before source implementation,
but the requirements are fixed:

- it must be explicit;
- absence must preserve upstream behavior exactly;
- it must describe a firmware ABI, not generic silicon capability;
- it must not be automatically enabled for all `sm8550-pmic-glink` compatible
  devices.

For a future upstreamable solution a dedicated compatible/firmware capability
mechanism may be more appropriate. The first Caihong bring-up patch is local and
must not pretend the Oplus ABI is universal.

## Oplus extension IDs to isolate

Only inside the explicitly gated extension namespace:

```text
USB SET opcode               0x33
USB_SET_PPS_VOLT             34   value: mV
USB_SET_PPS_CURR             35   value: mA

BAT SET opcode               0x31
OPLUS_BATT_SET_PDO           25   value: fixed voltage mV
```

Do not rename the generic upstream property 25. Keep the Oplus constant visibly
namespaced, e.g. `OPLUS_BATT_SET_PDO`, so code review cannot mistake it for
`BATT_CHG_CTRL_START_THR`.

## Existing request serialization must remain authoritative

All extension requests run under `battmgr->lock` and use the existing
`qcom_battmgr_request()` completion/service transport.

No second transaction lock/completion is permitted unless the entire battmgr
request layer is intentionally refactored.

The extension must therefore coexist with existing reads/notifications without
creating a parallel request path.

## SET response handling must be added explicitly

Linux v7.2's SM8350-family callback currently handles:

```text
BAT PROPERTY GET
USB PROPERTY GET
WLS PROPERTY GET
notification / charge-control responses
```

but not generic:

```text
BAT PROPERTY SET (0x31)
USB PROPERTY SET (0x33)
```

The default callback path warns about an unknown message and then reaches the
shared completion. That is not sufficient for a write API because a caller may
wake without a validated firmware result.

A future patch must add explicit SET response cases that:

1. validate response payload length;
2. decode returned property ID;
3. decode firmware result/return code;
4. reject a response for a different property than the pending extension
   request;
5. propagate firmware failure through `battmgr->error`;
6. complete the existing request only after the response has been validated.

## Pending request correlation

The wire protocol has no separate transaction ID in the common property
request. Because battmgr serializes normal requests with one mutex, only one
request should be outstanding, but the extension should still record what it
expects.

Conceptual private state:

```text
ext_pending
ext_expected_opcode
ext_expected_property
```

Before an extended SET:

```text
lock battmgr
set expected opcode/property
send request
wait through existing qcom_battmgr_request()
clear expected state
unlock battmgr
```

The callback's SET case must validate both opcode and property against that
state.

Do not expose this tracking state outside qcom_battmgr.

## Proposed narrow in-kernel API

Do not expose raw `opcode/property/value` to SC8547 code.

The future electrical API should be narrow and unit-safe. Conceptually:

```text
qcom_battmgr_oplus_pps_request(usb_psy, voltage_uv, current_ua)
qcom_battmgr_oplus_fixed_5v(usb_psy)
qcom_battmgr_oplus_pps_supported(usb_psy)
```

Names/signatures are not frozen. Required semantics are:

- API returns `-EOPNOTSUPP` unless the explicit Oplus firmware gate is enabled;
- API requires the passed power supply to be the battmgr USB supply owned by
  this instance;
- public units are Linux-standard microvolts/microamps;
- conversion to downstream mV/mA happens inside qcom_battmgr;
- no caller can pass an arbitrary property number;
- no SC8547 private structure is exposed or required.

## Request validation

Before sending any PPS SET, the future helper must at minimum require:

```text
service_up == true
USB online
UCSI reports an active USB-PD contract
partner PDO data proves a compatible PPS APDO
explicit Oplus firmware-extension gate
requested voltage/current within an authorization range
```

Do not use `qcom-battmgr-usb/usb_type` as the Caihong protocol gate.  Runtime
testing showed it selecting `SDP` while the same connection's PMIC-Glink UCSI
port reported `power_operation_mode=usb_power_delivery`.

The authorization range should not be a hard-coded guess from downstream
comments. It should ultimately be intersected with real source APDO capability
captured through Stage 6A/UCSI or another verified capability provider.

A development-only DT maximum may additionally provide a board/test ceiling,
but it must be **stricter than**, not a replacement for, adapter capability.

## Two-property PPS request atomicity

Downstream programs PPS voltage and current using two separate USB-property SET
transactions. A future helper therefore cannot claim true wire-level atomicity.

The first implementation must define partial-failure behavior. Minimum rule:

```text
SET voltage
  failure -> return, no current SET
SET current
  failure -> immediately attempt explicit fixed-5V fallback
```

Whether voltage-first should be retained for the first mainline experiment must
be reviewed against real Caihong Stage-6A data. Do not silently reorder the
sequence without documenting why.

The helper reports success only after both firmware SET responses succeed.
A later policy layer separately verifies observed VBUS before considering the
source contract usable.

## Explicit fallback helper

The fallback helper is electrically separate from PPS request:

```text
BAT PROPERTY SET opcode 0x31
Oplus property 25 = BATT_SET_PDO
value 5000 mV
```

It is available only under the Oplus firmware gate.

The helper must wait for and validate firmware acknowledgement exactly like PPS
SET. It must not treat Oplus PPS virtual-IC `EXIT` as source fallback; downstream
`EXIT` only changes the virtual IC online state.

## Detach and service-reset behavior

The extension must distinguish these cases:

### Source detached

If USB is no longer online, there is no source VBUS to return to 5 V. A fallback
request may legitimately fail because the charger service/source disappeared.
The policy layer must treat confirmed detach as a safe source-removal event,
while still ensuring both CPs are disabled.

### PMIC-Glink service down

If `service_up` becomes false during a request, the existing request may time out
because the PDR notification does not currently complete the pending request.
The future helper must propagate the timeout/service-down result and must not
report the contract as known.

The first patch does not need to redesign all battmgr recovery, but this behavior
must be tested before automatic CP policy is connected.

### Service returns

After service-up, source state/capabilities must be re-read. Do not assume the
previous PPS contract survived a charger-firmware restart.

## No direct userspace power_supply setter initially

The first Stage-6B bridge should **not** add writable generic power-supply
properties for arbitrary voltage/current.

Reasons:

- standard `VOLTAGE_MAX`/`CURRENT_MAX` semantics are not equivalent to this
  private PPS request ABI;
- a generic sysfs setter would make unsafe raw experimentation too easy;
- the bridge must be independently reviewable as a firmware transaction API.

A later development-only test consumer may expose tightly bounded manual
controls after this bridge itself compiles and its gate/range semantics are
reviewed.

## Source-only test consumer comes later

The first write-capable source experiment should use a separate, deliberately
small test/policy consumer above qcom_battmgr. It will:

- refuse operation if either SC8547 is enabled/switching;
- expose only a configured conservative request window;
- call the narrow qcom_battmgr API;
- verify source VBUS through qcom-battmgr plus both SC8547 ADCs;
- provide an explicit fallback command;
- never automatically enable a charge pump.

This source-only consumer is Stage 6B. The automatic CP/source state machine is
Stage 6C and must be a later commit.

## Kernel-tree patch test ladder

When source implementation is finally unblocked, split it into small commits:

### 6B-K0 — plumbing only

```text
- add explicit firmware-ABI gate
- add namespaced constants/private state
- no callable source SET operation
```

Test: boot with gate absent and present; verify no source/charging behavior
changes.

### 6B-K1 — SET response parser

```text
- add explicit 0x31/0x33 response validation under the gate
- still no userspace/OOT caller capable of sending PPS requests
```

Test: compile plus normal battmgr regression; no source write yet.

### 6B-K2 — fixed-5V helper

```text
- expose only gated fixed-5V fallback helper
```

First possible write test: while already on a normal PD source and CPs off,
request the known-safe fixed 5-V contract and verify ACK + observed ~5 V. Do not
implement PPS request in the same first-write commit.

The Caihong development tree now implements this test behind the separate
`oneplus,pps-fixed-5v` board-DT opt-in. It exposes only:

```text
/sys/class/power_supply/qcom-battmgr-usb/device/oneplus_fixed_5v
```

The attribute accepts only the literal value `5000`. Before sending the
downstream `BATT_SET_PDO` request it verifies USB-online state, issues a fresh
read-buffer request, and requires a real fixed 5-V PDO. The response path checks
the expected opcode and property ID and propagates the firmware return code.
It does not expose a voltage parameter, send a PPS request, or enable a charge
pump.

This K2 path passed on Caihong on 2026-09-02. Firmware acknowledged the 5-V
request, both SC8547 instances remained disabled and non-switching, and their
independent ADCs measured approximately 4.83 V. Their `vbus_low` status is
expected at fixed 5 V because the charge-pump input threshold is higher; it is
not a failed PD fallback while the pumps remain off.

### 6B-K3 — PPS request helper

Only after K2 fallback passes:

```text
- add bounded exact PPS request helper
- on partial failure call K2 fallback
```

First PPS experiment: both CPs off, one conservative APDO-supported request,
verify it, then immediately call K2 fallback and verify 5 V/basic charging.

The current Caihong development tree implements this first experiment behind
the additional board-DT opt-in `oneplus,pps-5v5-1a`. The write-only
`oneplus_pps_5v5_1a` attribute accepts only `1`, refreshes USB-online and source
capability state, requires both a fixed 5-V PDO and a PPS APDO covering 5.5 V at
1 A, then sends the vendor ABI's voltage property followed by its current
property. If the current-property transaction fails after voltage succeeds, it
immediately invokes the independently tested K2 fixed-5-V fallback. It never
enables either SC8547.

K3 passed on Caihong on 2026-09-02.  With both pumps reporting
`charge_enabled=0` and `switching=0`, the firmware acknowledged both SET
transactions.  Both independent SC8547 ADCs moved from approximately 4.93 V to
5.32 V for the 5.5-V/1-A request, then returned to approximately 4.93 V after
the K2 fixed-5-V command.  Pump IBUS remained approximately 20 mA and no pump
was enabled.  This proves the source-request and fallback wire ABI; it does not
yet prove charge-pump operation.

### 6B-K4 — source-only bounded ramp

The next local test remains source-only.  The additional board-DT opt-in
`oneplus,pps-ramp-9v-1a` exposes the write-only
`oneplus_pps_ramp_9v_1a` endpoint.  It accepts only `1`, requires a single
advertised PPS APDO to cover the entire 5.5--9.0-V range at 1 A, and requests
5.5, 6.0, 6.5, 7.0, 7.5, 8.0, 8.5 and 9.0 V in order.  Each voltage/current
pair uses the downstream voltage-then-current ABI and is followed by a 500-ms
settling interval.  Any failed transaction invokes the already-validated fixed
5-V fallback.

K4 deliberately has no connection to the SC8547 control API.  It leaves both
pumps off and holds the final 9-V/1-A source request only long enough for manual
telemetry collection; the operator then invokes `oneplus_fixed_5v` explicitly.

### 6B-U0 — source-only out-of-tree test consumer

Only after the bounded K4 source ramp has independent evidence:

```text
- bounded manual request/fallback interface
- both CPs required off
- combined telemetry verification
```

This split is intentional: fallback is tested before PPS elevation.

## Implementation gate

K0--K3 have passed the intended staged hardware gates.  K4 must pass with both
pumps off and a verified return to fixed 5 V before any source request is
connected to Stage-4 single-pump control.  Dual-pump and automatic-policy work
remain blocked behind a successful single-pump test and full fault/thermal
rollback handling.
