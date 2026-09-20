# Caihong out-of-tree driver status

Status date: 2026-09-20.

This repository is now the archive and development home for the Caihong
device-specific touchscreen, pogo and SC8547 work. The SC8547 charging work is
paused at the bounded Stage 7D13 checkpoint; it is not a production charging
implementation.

## SC8547

The physical driver contains the experimentally recovered SC8547/SC8547A
profiles, bounded pulse interface, local voltage/current/temperature guards,
watchdog handling and diagnostics used by the dual-pump tests. The last tested
controller state is Stage 7D13.

D13 ran for 591 half-second samples (4 min 55.5 s). Three bounded emergency
reductions recovered normally. The final sample then reported a one-sided
primary IBUS jump from about 0.98 A to 2.668125 A while the secondary remained
near 0.98 A. The coordinator and the primary physical worker both failed
closed, disabled the pumps and restored fixed 5 V. Voltage, path and thermal
guards remained within bounds. Available telemetry cannot distinguish a real
primary current-sharing transient from a primary ADC/status fault, and no
control change was made after this result.

The retained hardware image is:

```text
mainline-boot-v2-sc8547-stage7d13-bounded-emergency-recovery.img
sha256: a7d5635543be50ec52d4ec429a566e4944a63ffd4ff71670839fb653d1528b79
```

The complete experiment ledger remains in
`docs/sc8547-commit-test-matrix.md`. It is evidence, not a recommendation to
enable automatic charging.

## qcom_battmgr boundary

The charger firmware routes every BATTMGR-owner response to every client using
that owner. A second independent PMIC-Glink client would therefore race the
upstream `qcom_battmgr` request completion and is not a safe out-of-tree
solution.

The frozen experimental kernel implementation is preserved as
`patches/linux/0001-power-supply-qcom-battmgr-oneplus-pps-wip.patch`. It is
intentionally marked WIP: it contains the complete Stage 6/7 test policy and
must not be treated as a minimal or upstreamable core-driver change. A future
restart should keep only a small, serialized and unit-safe PPS transport bridge
in `qcom_battmgr`; source policy, SC8547 coordination and diagnostics belong in
a separate device module.

## Pogo keyboard and touchpad

The external `oneplus_pogo.ko` driver contains the latest UART protocol
implementation: RX framing/CRC validation, keyboard/media/touchpad input, host
TX, LED and touchpad commands, startup setup work and diagnostic sysfs state.

The current Caihong board data is:

```text
controller       QUPv3 wrapper 1, serial engine 7 (0x00a9c000)
UART TX/RX       GPIO62 / GPIO63, function qup1_se7
accessory power  GPIO100, active high
wake             GPIO137, active low, pull-up
TX enable        GPIO14, active high
baud             921600
touchpad         2764 x 1630, resolution 23 x 23
CRC init         0xc596
```

The board fragment is stored in
`dts/sm8650-oneplus-caihong-pogo.dtsi`. The Qualcomm GENI UART driver defaults
to DMA for a normal UART, while this accessory required the FIFO path during
bring-up. That selection cannot be implemented by a serdev child module after
the parent UART has probed. The minimal generic core patch is therefore kept
separately as
`patches/linux/0002-serial-qcom-geni-add-force-fifo-mode.patch`; all pogo
protocol and board logic remains out of tree.

The migrated external driver and DTS fragment still require a fresh hardware
regression before they should be described as production-ready.

## Restart criteria

SC8547 work should resume only when there is a way to distinguish primary
current from an ADC/status fault (for example independent input-current
measurement or a validated register/fault snapshot at the excursion). Until
then, further controller tuning risks adapting policy to an unidentified
measurement failure.
