# SC8547 Stage 5 dual-pump coordinator

This document defines the development-only dual-SC8547 bring-up for OnePlus
Pad Pro (`caihong`). Stage 5 is split into a read-only virtual coordinator (5A)
and an explicitly gated write-capable coordinator (5B).

Neither stage is an automatic charging policy. Stage 5B is laboratory control
only and must not be promoted to the stable/test branch merely because it
compiles.

## Downstream evidence and architecture

Caihong downstream describes two parallel CP paths:

- primary: QUP I2C hub 2, SC8547A, CP index 0;
- secondary: QUP I2C hub 0, SC8547-family slave, CP index 1;
- connection: parallel;
- downstream `main_cp = <0>`;
- downstream nominal input-current budget: 3000 mA for each CP.

Downstream also represents the pair through a separate `virtual_cp` layer.
This port follows the same layering: physical I2C devices remain independent
and a separate platform device references both of them.

The downstream 3000 mA values are recorded as evidence only. Stage 5 never
requests 6 A, negotiates a source contract, or treats those values as generic
safe limits.

## Physical-device prerequisites

Each physical device uses the normal SC8547-family driver and its existing
bring-up properties. Example labels:

```dts
cp_secondary: charger@6f {
    compatible = "southchip,sc8547";
    reg = <0x6f>;
    southchip,role = "secondary";

    /* Stage 3/4 experimental properties only when testing writes. */
};

cp_primary: charger@6f {
    compatible = "southchip,sc8547a";
    reg = <0x6f>;
    southchip,role = "primary";

    /* Stage 3/4 experimental properties only when testing writes. */
};
```

For Stage 5B, **both** physical nodes must independently have their Stage-3 raw
profile, Stage-3 opt-in, Stage-4 opt-in and explicit VBUS/VBAT safety windows.
The coordinator does not provide or override those per-chip safety parameters.

## Stage 5A: explicit pair + aggregate telemetry

The virtual node identifies the two physical devices by phandle:

```dts
sc8547_dual: charge-pump-coordinator {
    compatible = "southchip,sc8547-dual-experimental";
    southchip,primary = <&cp_primary>;
    southchip,secondary = <&cp_secondary>;
};
```

These `*-experimental` interfaces are local bring-up interfaces, not proposed
upstream bindings.

Phandles are required because both chips use I2C address `0x6f` on different
buses. The coordinator requires distinct clients, both bound to the `sc8547`
physical driver, with exact `primary` and `secondary` role strings.

### Stage-5A module and sysfs

The separate module is:

```text
sc8547_dual.ko
```

The platform device exposes:

```text
/sys/bus/platform/devices/<coordinator>/sc8547_dual/
```

Always-read-only attributes:

- `peer`
- `pair_state`
- `aggregate_ibus_ua`
- `last_result`

`pair_state` obtains both physical snapshots through the shared physical-driver
API. It reports, per pump:

- device ID and variant;
- Stage-3 initialization state;
- Stage-4 authorization state;
- enable/switching/mode;
- blocking-fault state;
- VBUS/VBAT/IBUS.

`aggregate_ibus_ua` is only the arithmetic sum of both IBUS ADC readings. It is
not battery current, a source-current request, a limit, or proof of equal current
sharing.

### Stage-5A test gate

With both CPs disabled:

```sh
modprobe sc8547_cp
modprobe sc8547_dual

cd /sys/bus/platform/devices/<coordinator>/sc8547_dual
cat peer
cat pair_state
cat aggregate_ibus_ua
cat last_result
```

Repeat unplugged and on a normal 5 V source. Confirm the correct physical pair,
plausible ADC values and exact aggregate-current arithmetic. Loading/reading
`sc8547_dual.ko` without Stage-5B opt-in must not create mode/enable controls or
change physical register state.

## Shared physical-driver safety API

Stage 5B does **not** reproduce Stage-4 SMBus/register write logic. The physical
`sc8547_cp` module exports a development-branch internal API declared in:

```text
charging/sc8547_api.h
```

The API provides:

- structured state snapshot;
- manual mode set while disabled;
- Stage-4 preflight without enabling;
- single-pump enable including Stage-4 post-enable validation;
- fail-closed single-pump disable.

The Stage-4 physical-device sysfs controls use the same functions. The virtual
coordinator therefore cannot bypass the physical driver's initialization,
variant, voltage-window, fault or post-enable checks.

`manual_preflight()` is intentionally observational. `manual_enable()` repeats
preflight immediately before the masked enable write so a two-pump coordinator
does not rely on stale earlier state.

## Stage 5B: gated dual start/stop

Writable virtual controls appear only when the coordinator node adds the third
explicit opt-in:

```dts
sc8547_dual: charge-pump-coordinator {
    compatible = "southchip,sc8547-dual-experimental";
    southchip,primary = <&cp_primary>;
    southchip,secondary = <&cp_secondary>;
    southchip,allow-experimental-dual-cp;
};
```

Without this property, `work_mode` and `dual_enable` are absent.

### Stage-5B sysfs

With the opt-in present:

```text
sc8547_dual/work_mode
sc8547_dual/dual_enable
sc8547_dual/last_result
```

`work_mode` accepts:

```text
2:1
bypass
```

It updates primary then secondary through the physical API and is allowed only
while both pumps are initialized, Stage-4-authorized, disabled, non-switching
and free of blocking faults. If the secondary mode write fails, the coordinator
best-effort restores the primary's previous mode; no pump has been started at
that point.

`dual_enable` accepts `0` or `1`.

`last_result` records the coordinator stage/reason and errno from the latest
write operation. It is diagnostic, not a stable ABI.

## Dual-enable preflight

Before the first enable write, Stage 5B requires:

1. the virtual Stage-5B opt-in;
2. a valid primary/secondary pair;
3. Stage-3 initialization complete on both;
4. Stage-4 authorization/window complete on both;
5. both pumps currently disabled and not switching;
6. no blocking fault in the initial snapshots;
7. both devices configured to the same work mode;
8. physical-driver Stage-4 preflight succeeds independently for primary and
   secondary.

If either preflight fails, neither pump is enabled.

## Start order and rollback

The implementation follows downstream `main_cp = <0>` with deterministic order:

1. snapshot both pumps;
2. preflight primary;
3. preflight secondary;
4. enable primary through `sc8547_manual_enable()`;
5. primary's physical driver waits the Stage-4 500 ms observation interval and
   validates switching/fault/window state;
6. only after primary succeeds, enable secondary the same way;
7. reread both snapshots and require both enabled, switching and fault-free.

Rollback rules:

- primary enable failure: best-effort disable primary;
- secondary enable failure: disable secondary, then primary;
- final pair validation failure: disable secondary, then primary;
- first implementation never continues in a degraded one-pump state after a
  dual-start failure.

## Stop order

Writing `0` to `dual_enable` performs reverse-order shutdown:

1. disable secondary;
2. disable primary.

Both attempts run even if the first reports an error. The first error is
returned after both shutdown attempts have been made.

The platform coordinator also performs the same best-effort dual stop on driver
remove and system shutdown when Stage-5B opt-in is present.

## Concurrency model

The virtual coordinator serializes its own operations with one coordinator
mutex. It does not expose or directly acquire physical-driver private mutexes.
Each physical API call takes the appropriate physical lock internally.

A concurrent physical Stage-4 sysfs operation may cause a coordinator operation
to fail, but it cannot bypass physical safety checks: `manual_enable()` always
rechecks preflight immediately before writing the enable bit. On partial dual
failure the coordinator attempts reverse-order fail-closed shutdown.

## Initial Stage-5B hardware test order

Do not start here until **each** pump has independently passed Stage 4.

Recommended sequence:

```sh
cd /sys/bus/platform/devices/<coordinator>/sc8547_dual

cat pair_state
cat last_result

# Select the already individually tested ratio.
echo '2:1' > work_mode
cat work_mode
cat pair_state

# Only with a controlled source and both physical Stage-4 windows satisfied:
echo 1 > dual_enable
cat dual_enable
cat pair_state
cat last_result

# Stop before changing anything else.
echo 0 > dual_enable
cat dual_enable
cat pair_state
cat last_result
```

Acceptance for a first successful run:

- before start, both physical pumps are initialized/authorized and off;
- selected modes match;
- primary reaches validated switching before secondary is attempted;
- after success, both report enable=1 and switching=1 without blocking faults;
- reported ADC values remain inside each physical Stage-4 safety window;
- stop returns both to enable=0;
- no unexpected PMIC/basic-charging regression is observed afterwards.

Also deliberately exercise failure paths with a setup that stays within safe
laboratory bounds: for example, remove Stage-4 authorization from one device and
confirm dual start is refused before any pump is enabled. Do **not** manufacture
OVP/OCP/thermal faults merely to test rollback.

## What Stage 5 still does not do

Stage 5 does not:

- request or negotiate USB-PD/PPS voltage/current;
- ramp source current;
- select battery charge-current targets;
- implement VOOC/SuperVOOC/UFCS;
- automatically decide when the second CP should enter/leave;
- continuously monitor faults via IRQ/policy work;
- implement thermal charging policy;
- claim downstream 3000 mA-per-CP values are generic safe limits.

Those responsibilities belong to Stage 6 or later.

## Merge rule

- Stage 5A read-only code can be tested/promoted independently after baseline
  telemetry is sound.
- Shared-API refactoring may be reviewed independently because it is intended to
  preserve Stage-4 behavior.
- Stage 5B write-capable coordination must not move to `main` until primary and
  secondary have each passed Stage 4 on real hardware.
- Focused Linux-v7.2 `W=1` CI is required, but successful compilation never
  replaces the hardware gates.
