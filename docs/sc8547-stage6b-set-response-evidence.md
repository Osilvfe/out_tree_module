# Stage 6B BATTMGR SET-response wire evidence

This document records the exact downstream request/response shape and ACK/error
handling relevant to the future qcom_battmgr Stage-6B K1 patch.

It is protocol evidence only. It does not implement any source-contract write.

## Downstream common property request

The OnePlus/Oplus SM8650 charger HAL uses the common request layout:

```c
struct battery_charger_req_msg {
    struct pmic_glink_hdr hdr;
    u32 battery_id;
    u32 property_id;
    u32 value;
};
```

For the operations currently relevant to Stage 6B:

```text
BAT property SET opcode = 0x31
USB property SET opcode = 0x33
```

The request therefore contains no independent transaction ID. The property ID
and opcode are the only request-specific selectors visible in this common
message shape.

## Downstream common response

The corresponding response layout is:

```c
struct battery_charger_resp_msg {
    struct pmic_glink_hdr hdr;
    u32 property_id;
    u32 value;
    u32 ret_code;
};
```

This confirms that a future mainline-side SET parser can validate at least:

```text
response opcode
response payload size
response property_id
response ret_code
```

against private pending-request state.

## Downstream SET callback behavior

The downstream message handler groups these operations together:

```text
BC_BATTERY_STATUS_SET = 0x31
BC_USB_STATUS_SET     = 0x33
BC_WLS_STATUS_SET     = 0x35
```

and calls its common `validate_message()` helper.

That helper checks:

1. incoming length equals `sizeof(struct battery_charger_resp_msg)`;
2. `ret_code == 0`.

A non-zero firmware return code sets `bcdev->error_prop = true` and the response
is not considered valid.

The downstream helper logs `property_id` on firmware error, but it does **not**
use that property ID to correlate the response to a separately recorded pending
property.

## Downstream request serialization

The generic downstream write path owns one transaction lock/completion:

```text
mutex_lock(rw_lock)
reinit_completion(ack)
error_prop = false
pmic_glink_write(...)
wait_for_completion_timeout(ack, BC_WAIT_TIME_MS)
...
mutex_unlock(rw_lock)
```

If the wait expires, the write fails with a timeout/error path.

If a response sets `error_prop`, the common write path clears the flag and
returns `-ENODATA` rather than success.

This independently supports the architectural conclusion that the BATTMGR owner
expects one serialized property transaction at a time.

## Why K1 should be stricter than downstream

For Caihong mainline bring-up there is no reason to preserve the downstream
parser's weaker correlation rule.

Linux `qcom_battmgr` already has one mutex/completion transaction model, and the
future Oplus extension is intentionally narrow. K1 should therefore store:

```text
ext_pending
ext_expected_opcode
ext_expected_property
```

before an extension request becomes possible.

A SET response is valid for the pending extension request only when all of these
are true:

```text
extension firmware gate enabled
ext_pending == true
response opcode == ext_expected_opcode
payload length == expected common response size
response property_id == ext_expected_property
response ret_code == 0
```

Any mismatch is an error; it must not be converted into successful request
completion.

## Callback-completion rule

The current upstream SM8350/SM8550 qcom_battmgr callback reaches the common ACK
completion for message classes it handles, but it does not explicitly validate
the Oplus private BAT/USB SET response semantics needed here.

K1 must make successful completion depend on the explicit SET-response case,
not on falling through an `unknown message` path.

Conceptually:

```text
receive 0x31/0x33
  |
  +-- extension disabled -> preserve upstream/non-extension behavior
  |
  +-- extension enabled but no matching ext_pending -> reject/log, do not
  |                                                   satisfy extension success
  |
  +-- matching pending request
        -> validate size/property/ret_code
        -> set battmgr error/result
        -> complete request
```

The exact interaction with ordinary upstream callback completion must be
reviewed carefully during K1 so an unexpected packet cannot wake a different
normal battmgr GET request.

## Firmware error mapping

The downstream implementation collapses a non-zero response `ret_code` into
`-ENODATA` in the generic write path.

The first mainline development patch may preserve a simple negative errno rather
than invent a detailed mapping for undocumented firmware codes. Requirements:

- non-zero firmware `ret_code` must never return success;
- the raw firmware result should be available in debug logging/evidence;
- the public Stage-6B helper returns a stable Linux error;
- no caller interprets a firmware error as a known source contract.

A richer mapping can be added later only if the firmware return-code ABI is
actually documented.

## Timeout semantics

A PMIC-Glink send success followed by ACK timeout is **not** proof that the
firmware did nothing. The electrical contract is unknown after timeout.

Therefore future K2/K3/U0 behavior remains:

```text
SET timeout
-> operation returns failure
-> both SC8547 pumps remain/are forced off
-> do not report requested VBUS as established
-> re-observe qcom-battmgr + both SC8547 VBUS ADCs
-> if attached and a later safe fallback operation is possible, use the
   separately validated K2 path
```

Do not automatically repeat an exact PPS elevation after a timeout.

## Relation to K0/K1/K2/K3

This evidence affects the future patch ladder as follows:

### K0

Add private expected opcode/property/pending state and the explicit firmware-ABI
gate, but no electrical caller.

### K1

Add explicit validated BAT/USB SET response handling using the exact common
response layout above. Still no electrical SET caller.

### K2

Only after K0/K1 and the hardware gate, expose fixed-5V fallback. Its response
must pass the K1 validation path.

### K3

Only after K2 hardware pass, expose bounded PPS voltage/current requests. Both
USB SET responses must independently pass the same K1 validation.

## Implementation gate remains closed

This protocol evidence narrows K1 but does not unlock K0-K3 source commits.
Real Stage-6A captures, runtime DT namespace confirmation and adapter APDO
capability evidence are still required by the canonical Stage-6B gate.
