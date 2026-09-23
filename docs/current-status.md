# Caihong out-of-tree driver status

Status date: 2026-09-23.

This repository carries Caihong drivers that are not yet upstream and archives
their bring-up evidence. The companion kernel tree remains the integration
tree; device-specific code is kept in dedicated Caihong files wherever the
generic Qualcomm drivers require a small hook. The SC8547 charging work is
paused at the bounded Stage 7D13 checkpoint and is not a production charging
implementation.

## NT36532E touchscreen and Wi-Fi baseline

The user confirmed Wi-Fi connection and normal use with
`mainline-boot-v2-wifi-deferred-hmt1-v9-official-bdf.img`. Resume is somewhat
slow; longer-term behavior remains to be observed.

The first NT36532E image was not a valid driver test: its actual `/init` did
not insert `nt36532e_ts.ko`. It also used a different WLAN init sequence and
different AMSS, M3 and BDF files from the working v9 image. Stage1 is withdrawn.

Stage2 is built directly from the pinned v9 image. It adds touch loading,
firmware, reset and pen DT properties while preserving the v9 kernel outside
its initramfs, all existing module/firmware records and the original WLAN
commands. Stage2 hardware logs now confirm cascade detection, firmware loading
(16 partitions), input-device registration and binding to `spi0.0`. The user
also confirmed increasing IRQ counts and coordinate/pressure/BTN_TOUCH events
in `evtest`, while taps in the graphical desktop still have no effect. Pen
has not been connected or tested.

Stage3 fixes a source-confirmed missing multitouch slot release, selects the
coordinate layout from firmware information, and adds read-only event counters
and the latest packet. It retains the same v9 Wi-Fi payload and init sequence.
The user confirmed normal desktop touch with stage3, then 10 simultaneous
points in a simple browser test. This is the working touch checkpoint, not
long-term validation. Later, touch stopped after sleep with no `evtest` output;
the user reported a PM error from `nt36532e_resume` returning `-110` (timeout).
That old error path leaves touch IRQ disabled.

Stage4 uses the existing DRM panel-follower API so firmware is uploaded after
panel preparation, including display blank/unblank. Work waits for both panel
and SPI device resume; the firmware is retained in RAM. This addresses a
source-confirmed ordering gap, while the exact hardware timeout cause and the
fix still need a device test. `touch_stats` now includes power/start counters
and errors. No kernel rebuild or WLAN payload change is involved.

Stage4 also fixes pen pressure/ranges/transforms and adds `pen_scan`,
`pen_stats`, and a passive `caihong-pen-status` helper. The user's pen is
OPN2402, but its vendor scan type, charge and Bluetooth state are unknown.
The original wireless charger is CPS8601 on I2C hub 3 at 0x41, with a separate
PMIC-Glink HBOOST dependency. Charging is not implemented by this stage.
Module compilation, host event/PM/diagnostic tests and final-image checks pass;
stage4 boot, Wi-Fi, touch/resume and pen still need hardware validation.
See [`nt36532e-bringup.md`](nt36532e-bringup.md) for reproducible packaging,
checksums and the logs needed to distinguish module insertion from probe.

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

### Post-refactor boot observation

During regression testing of the refactored image, one boot at about 15%
capacity and 3.73 V battery voltage failed before the continuous session could
start. The voltage ramp and pump preparation completed, but the primary SC8547
worker returned `-ERANGE`; the coordinator subsequently observed the primary
path already disabled and reported `-EIO`. Cleanup, both final-off checks and
the return to fixed 5 V all succeeded.

Charging started normally after rebooting the same image without a policy
change. This is therefore retained as an intermittent startup or
hardware-state observation, not evidence for relaxing a guard. The exact
physical sample that triggered `-ERANGE` was not captured, so the cause remains
unclassified.

## qcom_battmgr boundary

The charger firmware routes every BATTMGR-owner response to every client using
that owner. A second independent PMIC-Glink client would therefore race the
upstream `qcom_battmgr` request completion and is not a safe out-of-tree
solution.

The companion kernel tree now keeps the private definitions, state and policy
in `qcom_battmgr_caihong.h` and `qcom_battmgr_caihong_*.inc`; the generic
`qcom_battmgr.c` retains only the integration hooks required to share its
single serialized BATTMGR owner. The frozen pre-refactor implementation is
preserved as `patches/linux/0001-power-supply-qcom-battmgr-oneplus-pps-wip.patch`
for reproducibility. It is historical WIP, not the current source of truth or
an upstreamable core-driver patch.

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

The board data is stored directly in the companion kernel's
`sm8650-oneplus-caihong.dts`. The Qualcomm GENI UART driver defaults to DMA for
a normal UART, while this accessory required the FIFO path during bring-up.
That selection cannot be implemented by a serdev child module after the parent
UART has probed. The companion Caihong kernel tree therefore carries a small
generic DT-selected FIFO hook, with its implementation split into
`qcom_geni_serial_fifo.inc`; all pogo protocol logic remains in the Caihong
module and all board data remains in the Caihong DTS. The patch under
`patches/linux/` is the original reproducibility snapshot.

The migrated external driver and board integration still require a fresh
hardware regression before they should be described as production-ready.

## Restart criteria

SC8547 work should resume only when there is a way to distinguish primary
current from an ADC/status fault (for example independent input-current
measurement or a validated register/fault snapshot at the excursion). Until
then, further controller tuning risks adapting policy to an unidentified
measurement failure.
