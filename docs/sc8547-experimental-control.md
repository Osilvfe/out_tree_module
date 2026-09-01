# SC8547 experimental control interface

This file documents development-only write controls on the `sc8547-next`
branch. They are deliberately **not** part of the normal telemetry-only device
binding and should not be copied into a production/mainline DTS.

The design goal is fail-closed: merely enabling an SC8547 I2C node must never
make unverified protection/init/charge-pump writes possible.

## Stage ownership

- Stage 0-2: telemetry and decode only.
- Stage 3: reset/protection-init/watchdog writes, CP remains disabled.
- Stage 4: manual CP mode/enable, not implemented by the Stage-3 interface.

This document describes Stage 3.

## Explicit opt-in

Experimental write controls are exposed only when the node contains:

```dts
southchip,allow-experimental-control;
```

Without this property, the extra sysfs group is absent and the device remains
telemetry-only apart from ADC enable. The telemetry-only shutdown path also
performs no experimental CP/watchdog write.

The opt-in property is intentionally verbose. It is a laboratory bring-up flag,
not a hardware description property intended for an upstream binding.

## Supported silicon for Stage 3

Experimental control currently accepts only:

- SC8547
- SC8547A

SC8547D and unknown IDs remain readable through the telemetry path but reject
`apply_init`. This is intentional until their write-side compatibility has been
confirmed.

## Raw protection profile

Because the Oplus source has inconsistent comments/formulas for some project
values, Stage 3 does not convert requested millivolts/milliamps into register
codes. Instead, a test profile explicitly provides exact raw bytes.

### Required raw bytes

```dts
southchip,experimental-reg00 = <0xXX>; /* BAT OVP */
southchip,experimental-reg02 = <0xXX>; /* AC/VAC OVP */
southchip,experimental-reg04 = <0xXX>; /* VBUS OVP */
southchip,experimental-reg05 = <0xXX>; /* IBUS UCP/OCP */
```

All four are required before `apply_init` can succeed.

### Optional raw bytes

```dts
southchip,experimental-reg01 = <0xXX>; /* BAT OCP */
southchip,experimental-reg0d = <0xXX>; /* PMID2OUT UVP/OVP */
```

These are optional because the Oplus primary and secondary initialization
sequences do not write exactly the same register set. In particular, forcing a
primary-only write into the secondary path would defeat the purpose of keeping
the port conservative.

An optional property that is absent is left untouched by Stage 3. If present,
it is written and included in readback verification.

Every supplied value must fit in one byte (`0x00..0xff`). The driver never
silently substitutes downstream defaults for a missing required property.

## Why raw bytes are required

Caihong downstream DT uses:

```dts
ocp_reg = <0x0b>;
ovp_reg = <0x36>;
```

but these are project inputs, not necessarily direct register bytes for every
field/variant. The downstream drivers transform them differently for primary,
SC8547 secondary and SC8547A secondary paths. In addition, vendor comments and
header formulas disagree in some locations.

Therefore a Stage-3 DTS must be created only after deciding which exact raw
register image is being reproduced for a given pump and silicon variant.

## Experimental sysfs group

When opt-in is present, Stage 3 exposes a separate group:

```text
.../<bus>-006f/sc8547_experimental/
```

Attributes:

- `profile_raw` (read-only): required raw bytes and any optional bytes parsed
  from DT, or `incomplete`.
- `init_state` (read-only): `not_initialized` or `initialized`.
- `apply_init` (write-only): writing `1` performs the controlled init.
- `watchdog_ms` (read/write): watchdog timeout; accepted values are 0, 200,
  500, 1000, 5000 and 30000 ms.

`watchdog_ms` may be written only after `init_state` is `initialized`.

There is intentionally no `charge_enable` and no writable `charge_mode` in
Stage 3.

## Controlled init sequence

Writing `1` to `apply_init` performs the following sequence:

1. reject silicon variants other than SC8547/SC8547A;
2. reject an incomplete/invalid required raw profile;
3. clear `REG07[7]` with a masked update so CP is disabled;
4. disable the watchdog with a masked `REG09[2:0]` update;
5. issue the common `REG07[6]` register reset bit;
6. wait briefly for reset handling;
7. fail closed again after reset;
8. write required `REG00`, `REG02`, `REG04`, `REG05`;
9. write `REG01` and/or `REG0d` only when explicitly supplied;
10. fail closed again so CP and watchdog remain off;
11. enable ADC;
12. read back every register that Stage 3 wrote;
13. mark init complete only if every readback matches exactly.

Any failure leaves `init_state` at `not_initialized` and performs a best-effort
CP-disable/watchdog-disable sequence.

The sequence deliberately does **not** write VOOC/DPDM/UFCS registers such as
`0x21/0x22/0x2b/0x30/0x31/0x33` or SC8547A UFCS registers at `0x40+`.

It also avoids copying whole `REG07` bytes (`0x85`, `0x81`, etc.). CP enable is
controlled only by a masked bit update so variant-specific low bits survive.

## Watchdog control

Stage 3 changes only `REG09[2:0]` for watchdog timeout. It preserves:

- `REG09[7]` charge-pump ratio mode;
- POR/UCP-related status/mask bits in the same register.

Accepted mapping from the Oplus common header:

| timeout | code |
| ---: | ---: |
| disabled | 0 |
| 200 ms | 1 |
| 500 ms | 2 |
| 1000 ms | 3 |
| 5000 ms | 4 |
| 30000 ms | 5 |

Codes 6 and 7 are treated as reserved/unsupported.

The shutdown callback performs a best-effort CP-disable and watchdog-disable
only for nodes that explicitly enabled experimental control.

## Example development DTS

The following is a **template only**. Do not copy placeholder values or assume
that the raw downstream primary image is correct for the secondary pump.

```dts
charger@6f {
    compatible = "southchip,sc8547a";
    reg = <0x6f>;
    southchip,role = "primary";

    southchip,allow-experimental-control;

    /* required */
    southchip,experimental-reg00 = <0xXX>;
    southchip,experimental-reg02 = <0xXX>;
    southchip,experimental-reg04 = <0xXX>;
    southchip,experimental-reg05 = <0xXX>;

    /* optional: only reproduce them when justified for this path */
    southchip,experimental-reg01 = <0xXX>;
    southchip,experimental-reg0d = <0xXX>;
};
```

For a secondary test profile, omit optional registers that are not part of the
specific downstream init sequence being reproduced.

## Stage-3 test procedure

Before `apply_init`:

```sh
cd /sys/bus/i2c/devices/<bus>-006f
cat sc8547/register_dump
cat sc8547/protection_state
cat sc8547_experimental/profile_raw
cat sc8547_experimental/init_state
cat sc8547_experimental/watchdog_ms
```

Save the output.

Then, with a controlled lab setup and CP not in active direct-charge service:

```sh
echo 1 > sc8547_experimental/apply_init
cat sc8547_experimental/init_state
cat sc8547/register_dump
cat sc8547/protection_state
cat sc8547/faults
cat sc8547/charge_enabled
cat sc8547/switching
```

Acceptance criteria:

- `init_state` becomes `initialized`;
- `charge_enabled` stays `0`;
- `switching` stays `0`;
- every requested raw byte reads back exactly;
- omitted optional registers were not intentionally overwritten by the init;
- ADC values remain plausible;
- no unexpected OVP/OCP/UCP/thermal fault appears;
- normal PMIC/pmic-glink charging remains functional.

Only after these criteria pass on each pump should Stage 4 manual CP enable be
considered.

## Merge rule

Do not cherry-pick Stage-3 write-control commits to `main` before Stage 0-2
have been tested on the real tablet and the raw profile has been reviewed.
The documentation may be merged earlier.
