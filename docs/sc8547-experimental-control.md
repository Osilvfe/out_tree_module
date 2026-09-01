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
telemetry-only apart from ADC enable.

The opt-in property is intentionally verbose. It is a laboratory bring-up flag,
not a hardware description property intended for an upstream binding.

## Raw protection profile

Because the Oplus source has inconsistent comments/formulas for some project
values, Stage 3 does not convert requested millivolts/milliamps into register
codes. Instead, a test profile must explicitly provide the exact raw bytes to
write.

The planned properties are:

```dts
southchip,experimental-reg00 = <0xXX>; /* BAT OVP */
southchip,experimental-reg01 = <0xXX>; /* BAT OCP */
southchip,experimental-reg02 = <0xXX>; /* AC/VAC OVP */
southchip,experimental-reg04 = <0xXX>; /* VBUS OVP */
southchip,experimental-reg05 = <0xXX>; /* IBUS UCP/OCP */
southchip,experimental-reg0d = <0xXX>; /* PMID2OUT UVP/OVP */
```

All six properties are required before an experimental init can be executed.
Values greater than `0xff` are rejected.

The driver must not silently substitute downstream defaults when any property
is missing.

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

Planned attributes:

- `profile_raw` (read-only): exact raw profile parsed from DT, or `incomplete`.
- `init_state` (read-only): whether the experimental init has completed.
- `apply_init` (write-only): writing `1` performs the controlled init.
- `watchdog_ms` (read/write): watchdog timeout; accepted values are 0, 200,
  500, 1000, 5000 and 30000 ms.

There is intentionally no `charge_enable` and no writable `charge_mode` in
Stage 3.

## Controlled init sequence

Writing `1` to `apply_init` should perform the following sequence:

1. reject unknown silicon variants;
2. reject an incomplete raw profile;
3. clear `REG07[7]` with a masked update so CP is disabled;
4. disable the watchdog with a masked update;
5. issue the common register reset bit;
6. write only the generic charge-pump protection registers supplied in DT:
   `REG00`, `REG01`, `REG02`, `REG04`, `REG05`, `REG0d`;
7. keep CP disabled after the writes;
8. enable ADC;
9. read back the six protection registers;
10. mark init complete only when every readback matches the requested raw byte.

The sequence deliberately does **not** write VOOC/DPDM/UFCS registers such as
`0x21/0x22/0x2b/0x30/0x31/0x33` or SC8547A UFCS registers at `0x40+`.

It also avoids copying whole `REG07` bytes (`0x85`, `0x81`, etc.). CP enable is
controlled by a masked bit update so variant-specific low bits survive.

## Watchdog control

Stage 3 may change only `REG09[2:0]` for watchdog timeout. It must preserve:

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

## Example development DTS

The following is a **template only**. Do not copy placeholder values or assume
that the raw downstream primary image is correct for the secondary pump.

```dts
charger@6f {
    compatible = "southchip,sc8547a";
    reg = <0x6f>;
    southchip,role = "primary";

    southchip,allow-experimental-control;

    southchip,experimental-reg00 = <0xXX>;
    southchip,experimental-reg01 = <0xXX>;
    southchip,experimental-reg02 = <0xXX>;
    southchip,experimental-reg04 = <0xXX>;
    southchip,experimental-reg05 = <0xXX>;
    southchip,experimental-reg0d = <0xXX>;
};
```

## Stage-3 test procedure

Before `apply_init`:

```sh
cd /sys/bus/i2c/devices/<bus>-006f
cat sc8547/register_dump
cat sc8547/protection_state
cat sc8547_experimental/profile_raw
cat sc8547_experimental/init_state
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
- requested raw protection bytes read back exactly;
- ADC values remain plausible;
- no unexpected OVP/OCP/UCP/thermal fault appears;
- normal PMIC/pmic-glink charging remains functional.

Only after these criteria pass on each pump should Stage 4 manual CP enable be
considered.

## Merge rule

Do not cherry-pick the Stage-3 write-control commit to `main` before Stage 0-2
have been tested on the real tablet and the raw profile has been reviewed.
The documentation may be merged earlier.
