# SC8547 Stage 5 dual-pump coordinator

This document defines the development-only dual-SC8547 bring-up plan for
OnePlus Pad Pro (`caihong`). Stage 5 is intentionally split into a read-only
pairing/telemetry stage (5A) and a write-capable coordinator stage (5B).

The split is deliberate: pair discovery and aggregate diagnostics can be
validated before there is any code that starts two charge pumps together.

## Downstream evidence

Caihong downstream describes two parallel CP paths:

- primary: QUP I2C hub 2, SC8547A, CP index 0;
- secondary: QUP I2C hub 0, SC8547-family slave, CP index 1;
- connection: parallel;
- downstream `main_cp = <0>`;
- downstream nominal input-current budget: 3000 mA for each CP.

The standalone driver does not treat the 3000 mA values as permission to draw
6 A and Stage 5 does not negotiate any USB source current/voltage.

## Stage 5A: explicit pair + read-only aggregate telemetry

### DT relationship

Only the primary node declares its secondary peer:

```dts
cp_secondary: charger@6f {
    compatible = "southchip,sc8547";
    reg = <0x6f>;
    southchip,role = "secondary";
    /* Stage 0-4 properties as needed for that test stage. */
};

cp_primary: charger@6f {
    compatible = "southchip,sc8547a";
    reg = <0x6f>;
    southchip,role = "primary";

    southchip,experimental-secondary = <&cp_secondary>;
    /* Stage 0-4 properties as needed for that test stage. */
};
```

`experimental-secondary` is a local bring-up property, not a proposed upstream
binding. A phandle is used instead of bus-number assumptions because both chips
have the same I2C address (`0x6f`) on different buses.

The primary probe must not fail merely because the peer has not bound yet.
Stage-5A reads resolve the peer at access time. This preserves baseline primary
telemetry when the secondary bus/driver is unavailable.

### Stage-5A sysfs group

When a primary node has `southchip,experimental-secondary`, expose:

```text
.../<primary-bus>-006f/sc8547_dual/
```

Read-only attributes:

- `peer`: primary/secondary role, variant and device identity; reports
  `unavailable` when the secondary device is not yet bound to this driver;
- `pair_state`: one snapshot containing enable/switching/fault state and
  VBUS/VBAT/IBUS for each pump;
- `aggregate_ibus_ua`: diagnostic sum of primary and secondary IBUS ADC values.

The aggregate current is only a diagnostic arithmetic sum. It is not a current
limit, a requested current, battery charge current, or proof that the two paths
share current evenly.

### Pair validation

A pair is considered ready only when:

1. the peer phandle resolves to an I2C client;
2. that client is bound to this SC8547 driver and has driver data;
3. primary role string is exactly `primary`;
4. peer role string is exactly `secondary`;
5. peer is not the same device;
6. both silicon variants are known to the driver.

Stage 5A performs no new protection, mode or CP-enable write.

### Stage-5A test gate

With both CPs disabled, collect:

```sh
cd /sys/bus/i2c/devices/<primary-bus>-006f
cat sc8547_dual/peer
cat sc8547_dual/pair_state
cat sc8547_dual/aggregate_ibus_ua
```

Repeat unplugged and with a normal 5 V source. Verify that the output clearly
identifies the correct buses/roles/variants, both ADC sets remain plausible and
aggregate IBUS is consistent with the two reported per-pump IBUS values.

Stage 5A may be promoted independently because it adds no dual-pump write path.

## Stage 5B: gated dual start/stop

Stage 5B must not expose writable dual controls unless the primary has a third
explicit opt-in:

```dts
southchip,allow-experimental-dual-cp;
```

This property is valid only together with:

```dts
southchip,experimental-secondary = <&cp_secondary>;
```

Both devices must independently have the Stage-3 and Stage-4 prerequisites
required for manual CP control.

### Planned Stage-5B controls

The primary's `sc8547_dual/` group gains development-only controls:

- `work_mode`: common requested `2:1` or `bypass` mode;
- `dual_enable`: `0` or `1`;
- `last_result`: last coordinator result/reason for bring-up diagnostics.

There is no automatic charger policy.

### Dual-enable preflight

Before the first enable write, require:

1. explicit Stage-5B opt-in;
2. valid and bound primary/secondary pair;
3. supported silicon on both devices;
4. Stage-3 `init_done` on both;
5. Stage-4 CP-enable authorization/window complete on both;
6. both CPs currently disabled and not switching;
7. both devices pass the existing Stage-4 preflight independently;
8. both devices are configured for the same requested work mode.

If either preflight fails, neither CP is enabled.

### Start order

First implementation uses a deterministic order consistent with downstream
`main_cp = <0>`:

1. run both preflights;
2. start primary only;
3. wait for primary Stage-4 post-enable validation;
4. if primary fails, disable primary and stop;
5. start secondary;
6. wait for secondary Stage-4 post-enable validation;
7. if secondary fails, disable secondary and then primary;
8. reread both fault/switching states before reporting success.

There is intentionally no first-version degraded one-pump fallback after a
secondary failure. Initial dual bring-up fails closed to both pumps off.

### Stop order

Dual stop runs in reverse:

1. disable secondary;
2. verify its enable bit cleared;
3. disable primary;
4. verify its enable bit cleared.

Best effort is used to turn both off even if the first disable reports an I2C
error. The first error is returned after both shutdown attempts have run.

### Concurrency/locking rule

Coordinator operations always lock primary first and secondary second. Existing
single-device controls lock only their own device. No code path may take the
secondary lock and then attempt to acquire the primary lock.

### Fault policy

During explicit coordinator operations/checks, any blocking Stage-4 fault on
either pump causes both pumps to be disabled. Continuous monitoring belongs to
a later policy/IRQ stage; Stage 5B alone is still a laboratory control surface.

## What Stage 5 does not do

Stage 5 does not:

- request USB-PD/PPS voltage/current;
- ramp source current;
- select a battery charge-current target;
- implement VOOC/SuperVOOC/UFCS;
- automatically decide when a second CP should enter/leave;
- implement thermal policy beyond rejecting existing blocking fault states;
- claim the downstream 3000 mA-per-CP values are safe generic limits.

Those policy decisions belong to Stage 6 or later.

## Merge/test rule

- Stage 5A read-only pairing may be tested/cherry-picked after the earlier
  telemetry stages are sound.
- Stage 5B write-capable coordination must not move to `main` until primary and
  secondary have each independently passed Stage 4 on real hardware.
- Linux-v7.2 focused `W=1` CI is necessary but never substitutes for the
  hardware gates.