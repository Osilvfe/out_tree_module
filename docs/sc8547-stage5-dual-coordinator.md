# SC8547 Stage 5 dual-pump coordinator

This document defines the development-only dual-SC8547 bring-up plan for
OnePlus Pad Pro (`caihong`). Stage 5 is split into a read-only virtual
coordinator (5A) and a write-capable coordinator (5B).

The split is deliberate: pair discovery and aggregate diagnostics can be
validated before there is any code that starts two charge pumps together.

## Downstream evidence and architecture choice

Caihong downstream describes two parallel CP paths:

- primary: QUP I2C hub 2, SC8547A, CP index 0;
- secondary: QUP I2C hub 0, SC8547-family slave, CP index 1;
- connection: parallel;
- downstream `main_cp = <0>`;
- downstream nominal input-current budget: 3000 mA for each CP.

Downstream also represents the pair through a separate `virtual_cp` layer.
Stage 5 follows that architectural idea: the two physical I2C devices remain
independent, while a separate platform device references both of them.

The standalone driver does not treat the downstream 3000 mA values as
permission to draw 6 A and Stage 5 does not negotiate USB source voltage or
current.

## Stage 5A: explicit pair + read-only aggregate telemetry

### DT relationship

The two physical nodes remain normal SC8547-family devices:

```dts
cp_secondary: charger@6f {
    compatible = "southchip,sc8547";
    reg = <0x6f>;
    southchip,role = "secondary";
    /* Stage 0-4 development properties only when needed. */
};

cp_primary: charger@6f {
    compatible = "southchip,sc8547a";
    reg = <0x6f>;
    southchip,role = "primary";
    /* Stage 0-4 development properties only when needed. */
};
```

A separate coordinator node identifies the pair:

```dts
sc8547_dual: charge-pump-coordinator {
    compatible = "southchip,sc8547-dual-experimental";
    southchip,primary = <&cp_primary>;
    southchip,secondary = <&cp_secondary>;
};
```

All three `southchip,*-experimental` concepts used in this development branch
are local bring-up interfaces, not proposed upstream bindings.

Phandles are used instead of bus-number/address assumptions because both chips
use I2C address `0x6f` on different buses.

The coordinator probe does not require both I2C clients to be available. Peer
resolution occurs when telemetry is read, so the physical CP drivers continue
to probe and expose baseline telemetry independently.

### Module and sysfs layout

Stage 5A builds a separate module:

```text
sc8547_dual.ko
```

The coordinator platform device exposes:

```text
/sys/bus/platform/devices/<coordinator>/sc8547_dual/
```

Read-only attributes:

- `peer`: physical device names, IDs and coarse silicon-variant labels;
- `pair_state`: one snapshot containing enable/switching/mode/fault state and
  VBUS/VBAT/IBUS for both pumps;
- `aggregate_ibus_ua`: arithmetic sum of the two IBUS ADC values.

The aggregate current is diagnostic only. It is not a current limit, a source
request, battery current, or proof that the two parallel paths share current
evenly.

### Pair validation

A Stage-5A pair is usable only when:

1. both phandles resolve to I2C clients;
2. the two phandles do not refer to the same device;
3. both clients are currently bound to the `sc8547` physical driver;
4. primary has `southchip,role = "primary"`;
5. secondary has `southchip,role = "secondary"`;
6. both clients respond to the common status/ADC register reads.

If either physical device is unavailable/unbound, `peer` reports unavailable
and the snapshot/current attributes fail rather than inventing data.

Stage 5A performs no protection, mode, watchdog or CP-enable write.

### Stage-5A test gate

With both CPs disabled, load both modules and collect:

```sh
modprobe sc8547_cp
modprobe sc8547_dual

cd /sys/bus/platform/devices/<coordinator>/sc8547_dual
cat peer
cat pair_state
cat aggregate_ibus_ua
```

Repeat unplugged and with a normal 5 V source. Verify that:

- the physical devices/buses correspond to the intended primary/secondary;
- IDs/variant labels are plausible;
- both ADC sets are plausible;
- `aggregate_ibus_ua` equals the two displayed per-pump IBUS values summed;
- no control/protection register changes merely from loading/reading the
  coordinator.

Stage 5A may be promoted independently because it adds no dual-pump write path.

## Stage 5B: gated dual start/stop

Stage 5B will extend the **coordinator node**, not either physical I2C node.
Writable controls must not appear unless the coordinator contains a third
explicit opt-in:

```dts
sc8547_dual: charge-pump-coordinator {
    compatible = "southchip,sc8547-dual-experimental";
    southchip,primary = <&cp_primary>;
    southchip,secondary = <&cp_secondary>;
    southchip,allow-experimental-dual-cp;
};
```

Both physical devices must independently have the Stage-3/4 properties needed
for their own controlled initialization and single-pump authorization windows.

### Stage-5B physical-driver API requirement

Stage 5B must not duplicate the Stage-4 safety logic by performing arbitrary
SMBus writes in the coordinator. Before implementing dual writes, the physical
`sc8547_cp` module will expose a small internal/exported API for:

- obtaining a referenced SC8547 device from an I2C client;
- checking whether Stage-3 init and Stage-4 authorization are ready;
- setting mode while disabled;
- running the existing Stage-4 preflight;
- enabling one pump and performing its post-enable validation;
- disabling one pump fail-closed;
- reading a structured state snapshot.

The sysfs implementation and coordinator will both use the same helpers. There
must be one safety implementation, not two diverging copies.

### Planned Stage-5B controls

The coordinator `sc8547_dual/` group gains development-only controls:

- `work_mode`: common requested `2:1` or `bypass` mode;
- `dual_enable`: `0` or `1`;
- `last_result`: last coordinator operation/result for bring-up diagnostics.

There is no automatic charger policy.

### Dual-enable preflight

Before the first enable write, require:

1. explicit Stage-5B opt-in;
2. valid/bound physical pair;
3. supported silicon on both devices;
4. Stage-3 `init_done` on both;
5. Stage-4 CP-enable authorization/window complete on both;
6. both CPs disabled and not switching;
7. both existing Stage-4 preflights pass independently;
8. both devices are configured for the same requested work mode.

If either preflight fails, neither pump is enabled.

### Start order

The first implementation uses a deterministic order consistent with downstream
`main_cp = <0>`:

1. run both preflights;
2. start primary only;
3. wait for primary's existing Stage-4 post-enable validation;
4. if primary fails, disable primary and stop;
5. start secondary;
6. wait for secondary's existing Stage-4 post-enable validation;
7. if secondary fails, disable secondary and then primary;
8. reread both states before reporting success.

There is intentionally no first-version degraded one-pump fallback after a
secondary failure. Initial dual bring-up fails closed to both pumps off.

### Stop order

Dual stop runs in reverse:

1. disable secondary;
2. verify its enable bit cleared;
3. disable primary;
4. verify its enable bit cleared.

Both disable attempts run even if the first reports an I2C error. The first
error is returned only after both shutdown attempts have been made.

### Concurrency/locking rule

Coordinator write operations must use one documented lock order: physical
primary first, physical secondary second. No helper may hold the secondary lock
and then acquire the primary lock.

### Fault policy

During explicit coordinator operations/checks, any blocking Stage-4 fault on
either pump causes both pumps to be disabled. Continuous monitoring belongs to
a later IRQ/policy stage; Stage 5B alone remains a laboratory control surface.

## What Stage 5 does not do

Stage 5 does not:

- request USB-PD/PPS voltage/current;
- ramp source current;
- choose battery charge-current targets;
- implement VOOC/SuperVOOC/UFCS;
- automatically decide when the second CP should enter/leave;
- implement a full thermal policy;
- claim the downstream 3000 mA-per-CP values are safe generic limits.

Those policy decisions belong to Stage 6 or later.

## Merge/test rule

- Stage 5A read-only coordinator may be tested/cherry-picked after the earlier
  telemetry stages are sound.
- Stage 5B write-capable coordination must not move to `main` until primary and
  secondary have each independently passed Stage 4 on real hardware.
- Linux-v7.2 focused `W=1` CI is necessary but never substitutes for hardware
  gates.
