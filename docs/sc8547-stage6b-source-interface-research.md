# SC8547 Stage 6B source-interface research

This document records the **write-side interface evidence** for Stage 6B. It is
research/documentation only. It does not authorize or implement any USB source
contract change.

The operator-facing stage/cherry-pick/test sequence is:

```text
docs/sc8547-stage-test-plan.md
```

The broader Stage-6 architecture is:

```text
docs/sc8547-stage6-pd-pps-policy.md
```

## Why this document exists

Stage 6B must request source voltage/current while both SC8547 pumps are still
off. The mainline and downstream trees expose several interfaces that can look
similar at first sight:

- Qualcomm `qcom_battmgr` USB properties;
- Qualcomm PMIC-Glink UCSI;
- UCSI `GET_PDOS`;
- UCSI `SET_PDOS`;
- UCSI `SET_POWER_LEVEL`;
- Oplus fixed-PDO selection;
- Oplus PPS voltage/current SET properties;
- Oplus private PPS read-buffer capability query.

They are **not interchangeable**. This file records the boundary of each one so
future code does not accidentally use a capability-management command as a PPS
RDO request.

## Current conclusion

At the current checkpoint:

1. mainline UCSI is useful for **read-only partner PDO/APDO discovery** on a PPM
   that advertises PDO details;
2. UCSI `SET_PDOS` changes the local connector's advertised source/sink
   capability PDO set; it is **not** an exact request for a partner PPS output;
3. UCSI `SET_POWER_LEVEL` can ask the PPM to limit/renegotiate maximum sink or
   source power, but its PD control is expressed as a maximum power level, not
   the exact `(mV, mA)` programmable RDO required by the downstream PPS loop;
4. Linux v7.2 exposes no normal high-level UCSI power-supply setter for exact
   PPS voltage/current;
5. the Oplus/QCOM charger-firmware path does contain exact PPS `(mV, mA)` SET
   operations, but those are downstream/private ABI evidence and must not be
   copied blindly into an out-of-tree bridge;
6. therefore **Stage 6B remains unimplemented**.

## Mainline Linux v7.2: Qualcomm PMIC-Glink UCSI transport

`drivers/usb/typec/ucsi/ucsi_glink.c` implements a generic UCSI transport over
Qualcomm PMIC Glink.

Transport messages are:

```text
UC_UCSI_READ_BUF_REQ   = 0x11
UC_UCSI_WRITE_BUF_REQ  = 0x12
UC_UCSI_USBC_NOTIFY_IND = 0x13
```

The transport passes UCSI CONTROL and message data to the charger/PD firmware,
waits for explicit read/write completion, and reports timeout/error to the UCSI
core. There is no Caihong-specific PPS request helper in this transport.

This is important architecturally: Qualcomm already gives Linux a standards-
based PD-control channel, so private charger-firmware messages should be added
only where UCSI cannot express the required operation.

## Mainline UCSI: partner PDO/APDO observation

When the PPM advertises:

```text
UCSI_CAP_PDO_DETAILS
```

### Caihong runtime result

Caihong's PMIC-Glink UCSI `GET_CAPABILITY` response decoded as:

```text
attributes      = 0x00004046
num_connectors  = 1
features        = 0x0000
BC version      = 1.20
PD version      = 3.0
Type-C version  = 1.3
```

The charger can establish a PD connection, and UCSI reports a valid RDO
operating current, but `UCSI_CAP_PDO_DETAILS` is absent.  Consequently
`src_pdos[]` remains empty, the UCSI power-supply voltage/max fields read zero,
and no partner `source-capabilities`/PPS APDO tree is registered.  This is a
firmware capability limitation, not evidence that VBUS is actually zero.

For Caihong, Stage 6A therefore has to use the downstream read-only PPS
capability query (or another independently proven firmware interface) to learn
APDO limits.  It must not fabricate PDOs from the adapter name or from the RDO
current alone.

UCSI core sends `UCSI_GET_PDOS` for the partner source PDOs.

The returned PDOs are registered through the Linux USB Power Delivery class.
Therefore a working Caihong UCSI implementation can expose adapter capabilities
under:

```text
/sys/class/usb_power_delivery/
```

For a PPS APDO, Linux v7.2 creates a `programmable_supply` PDO device with
read-only attributes including:

```text
minimum_voltage
maximum_voltage
maximum_current
pps_power_limited       # source PPS APDO
```

This is potentially better Stage-6A/6B capability evidence than the Oplus
private `PPS_OPCODE_READ_BUFFER`, because it uses the standard Type-C/PD stack.

### Required Stage-6A capture

On a PPS-capable adapter, before any future source SET work:

```sh
find /sys/class/usb_power_delivery -maxdepth 5 -type f -print -exec cat {} \;
```

If the class is exposed through symlinks/directories in a way that `find -type
f` misses, also record:

```sh
find -L /sys/class/usb_power_delivery -maxdepth 5 -print
```

and manually capture all readable files below the partner
`source-capabilities` tree.

We specifically need to know whether Caihong's PPM returns the PPS APDO minimum
voltage, maximum voltage and maximum current expected from the adapter.

## UCSI SET_PDOS is not a PPS output request

Linux v7.2 contains `UCSI_SET_PDOS` support through the UCSI debugfs/raw command
path.

The command's purpose is to update the **source or sink capabilities PDOs on a
connector**. Other UCSI implementations use it to program the local sink-PDO
set reflecting board power limits or the local source-PDO set.

Therefore this is the wrong primitive for:

```text
request adapter output = 5500 mV @ 1000 mA
```

It may affect what the local PPM subsequently negotiates, but it does not carry
the programmable PPS requested voltage/current pair used in a PPS RDO.

Stage 6B must not use `SET_PDOS` as a shortcut for Oplus
`USB_SET_PPS_VOLT/CURR`.

## UCSI SET_POWER_LEVEL is closer, but still not exact PPS control

UCSI `SET_POWER_LEVEL` allows the OPM to select source/sink direction and a
maximum negotiable USB-PD power level. For PD the maximum-power field is in
0.5-W units and the PPM may renegotiate the current connection.

This may be useful as a **policy ceiling**, but it cannot describe the exact
programmable voltage/current tuple needed by the downstream PPS loop. For
example, the Oplus first PPS request is explicitly around:

```text
5500 mV @ 800 mA   (Oplus-adapter path)
or
5500 mV @ 1000 mA  (third-party PPS path)
```

A maximum-power request alone cannot guarantee that the PPM selects 5.5 V, nor
can it implement the later tens-of-mV PPS voltage ramp.

Linux v7.2 also does not expose `SET_POWER_LEVEL` as a normal power-supply
setter; it is available only through lower/raw UCSI command machinery.

Conclusion: do not build Stage 6B around `SET_POWER_LEVEL` unless later hardware
experiments prove the Qualcomm PPM provides an additional exact-PPS policy
mechanism. It can at most be considered as a future upper power cap.

## UCSI power-supply limitations

The UCSI port power supply is read-only in Linux v7.2. It exposes source power
information such as voltage/current and USB type, but has no `.set_property`.

Also, its current `VOLTAGE_NOW`/`CURRENT_NOW` implementation is based on the
standard connector status/RDO representation and its source-voltage helper uses
fixed-PDO decoding. Do not treat the UCSI power-supply view alone as an exact
PPS-RDO observer until Caihong data has been compared with:

- `qcom-battmgr-usb`;
- SC8547 VBUS ADCs;
- the USB-PD capability tree;
- actual adapter behavior.

## Oplus/QCOM downstream exact PPS request path

For the non-SoCCP path, downstream identifies USB-property SET opcode:

```text
BC_USB_STATUS_SET = 0x33
```

and the relevant USB property IDs are:

```text
USB_SET_PPS_VOLT = 34
USB_SET_PPS_CURR = 35
```

The callers pass voltage in mV and current in mA.

The generic downstream property-write request contains:

```text
property_id
battery_id = 0
value
owner = MSG_OWNER_BC
type = MSG_TYPE_REQ_RESP
opcode = USB opcode_set
```

and waits for charger-firmware ACK. A firmware non-zero return code is treated
as an error; a missing completion becomes timeout.

This is much closer to the exact semantics Stage 6B eventually needs than
UCSI `SET_POWER_LEVEL`, but it is not an upstream ABI contract.

## Caihong SoCCP branch evidence

Source include-tree inspection for project 23926 currently shows:

```text
caihong-23926-pineapple-overlay.dts
  -> pineapple-mtp-overlay.dts / pineapple-mtp.dtsi
  -> pineapple_overlay_common.dtsi
  -> caihong_overlay_common.dtsi
  -> oplus-chg-23926.dtsi
```

The inspected `battery_charger` declarations/overrides do not set
`oplus,soccp_support`. This strongly suggests the Pad Pro uses the non-SoCCP
property namespace above.

This is **not enough for a write test**. Before Stage 6B code sends any private
property, verify the final compiled DTB or the running device tree explicitly.

Required future capture example:

```sh
find /proc/device-tree -name 'soccp_support' -o -name '*soccp*' -print
```

and inspect the actual `battery_charger` node properties. Use an equivalent
`dtc`/`fdtdump` inspection of the exact DTB/DTBO being flashed as a second
check.

## Oplus downstream capability query

Downstream PPS APDO information is not read through normal `power_supply`
properties. It uses a dedicated PMIC-Glink request:

```text
PPS_OPCODE_READ_BUFFER = 0x10004
```

with a dedicated completion/timeout path.

The returned buffer is interpreted as:

```text
word 0: imax
word 1: vmax
word 2+: PDO data
```

The Pad Pro source used to verify this ABI is the shallow clone at:

```text
external/oneplus-sm8650-pad-pro
branch: oneplus/sm8650_b_16.0.0_pad_pro
commit: 0a4a245d2786bf25a9683cfc3839d6ee5b8562e4
```

The verified wire definitions are:

```text
owner                         PMIC_GLINK_OWNER_BATTMGR (32778)
type                          request/response (1)
opcode                        0x10004
request payload               u32 data_size = 512 bytes
response payload              u32 data[128], u32 data_size
downstream timeout            500 ms
maximum consumed PDO entries  7
```

Mainline Caihong now has a DT-gated, read-only diagnostic implementation in
`drivers/power/supply/qcom_battmgr.c`. It uses the existing battery-manager
Glink client and serializes against normal battery-property requests. Reading
the following attribute sends exactly one capability request:

```sh
cat /sys/class/power_supply/qcom-battmgr-usb/device/oneplus_pps_capabilities
```

The output contains the raw `imax`, `vmax`, and up to seven raw PDO words, plus
a convenience decode for fixed PDOs and PPS APDOs. This interface has no write
attribute, sends no property SET request, changes no source contract, and does
not enable either charge pump.

### Caihong Stage-6A hardware result

The read-buffer request was verified on Caihong on 2026-09-02. With the tested
PD/PPS adapter connected, firmware returned:

```text
fixed:  5 V / 3 A
fixed:  9 V / 3 A
fixed: 12 V / 3 A
fixed: 15 V / 3 A
fixed: 20 V / 5 A
PPS:    5-11 V / 5 A
PPS:    5-20 V / 5 A
```

After disconnect, the same request completed successfully but all seven PDO
words were zero. This proves that the buffer reflects source attachment rather
than a hard-coded adapter table. `imax` and `vmax` were zero in both captures;
they must not be used as capability bounds before their downstream meaning is
independently established.

This private read-buffer protocol is useful as evidence for what the Oplus
charger firmware supplies to its PPS policy. It should **not** be reimplemented
if Caihong UCSI `GET_PDOS` already exposes the same APDO capability information
through the standard PD class.

## Oplus adapter authentication is not a generic PPS prerequisite

The non-SoCCP downstream USB property namespace also has the Oplus PPS
adapter-authentication path. That is used to classify/verify an Oplus adapter
and select Oplus-specific policy/current behavior.

Third-party standards-compliant PPS operation does not require an Oplus
proprietary authentication success. A generic Stage 6B bridge must therefore
not make Oplus authentication a prerequisite for basic USB-PD PPS.

Keep proprietary/Oplus adapter handling separate from generic PPS control.

## Stage 6B interface decision tree

Use this order when Stage-6A hardware evidence arrives:

```text
1. Does Caihong UCSI expose partner PDO/APDO capabilities correctly?
      yes -> use standard USB-PD capability tree for discovery
      no  -> investigate PPM/UCSI limitation before copying private read-buffer

2. Does mainline UCSI expose an exact sink PPS (mV,mA) request operation?
      currently no known high-level API

3. Can SET_POWER_LEVEL alone satisfy the required exact PPS sequence?
      no: it controls max power, not exact programmable voltage/current

4. Therefore, is a Qualcomm charger-firmware bridge required?
      likely yes for exact PPS request, unless a better standard API is found

5. If required, should the bridge live in sc8547 code?
      no
      it belongs beside/inside the Qualcomm charger-firmware integration and
      must remain electrically independent of the charge-pump driver
```

## Proposed Stage 6B layering if a private bridge is ultimately required

Do not put PMIC-Glink wire-format code into `sc8547_cp.ko` or
`sc8547_dual.ko`.

Preferred layering:

```text
Qualcomm/Oplus source-contract bridge
  - capability read
  - PPS request(mV, mA)
  - fixed/basic fallback
  - firmware/service state
  - explicit ACK/error
          |
          v
Stage-6 policy driver
          |
          v
sc8547_dual
          |
          v
physical SC8547 pair
```

The bridge must be testable with both charge pumps absent/off.

## Requirements for the first future write-capable commit

Before creating a Stage-6B functional commit, its preceding documentation must
state all of the following:

- exact transport and ownership;
- exact command/opcode/property;
- units and valid ranges;
- how source capabilities constrain the request;
- positive ACK condition;
- negative firmware return behavior;
- timeout behavior;
- PMIC-Glink/service-down behavior;
- detach behavior;
- exact basic/5-V fallback operation;
- first conservative test request;
- how both SC8547 VBUS ADCs will verify the source result while CPs are off.

If any item is still guessed, Stage 6B is not ready to implement.

## First Stage-6B hardware gate remains source-only

The first future Stage-6B experiment must not enable either charge pump:

```text
CP primary OFF
CP secondary OFF
        |
        v
capture standard UCSI/USB-PD capabilities
        |
        v
verify current basic source state
        |
        v
one conservative verified source request
        |
        v
require firmware/protocol success
        |
        v
verify VBUS using qcom-battmgr + both SC8547 ADCs
        |
        v
explicitly return to basic/5-V state
        |
        v
verify normal PMIC charging recovery
```

Only after this is repeatedly reliable can a later Stage 6C consume the source
bridge.
