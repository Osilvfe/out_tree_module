# Caihong out-of-tree driver status

Status date: 2026-09-24.

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
source-confirmed ordering gap. The user has now confirmed that sleep and
touch recovery work with stage4. This validates the tested wake cycle, without
establishing the exact old timeout cause or long-term stability. `touch_stats`
includes power/start counters and errors. No kernel rebuild or WLAN payload
change is involved.

Stage4 also fixes pen pressure/ranges/transforms and adds `pen_scan`,
`pen_stats`, and a passive `caihong-pen-status` helper. The user's pen is
OPN2402. The user now confirms the pen has power and charges under another
system; its vendor scan type remains unknown. The Linux Bluetooth controller
is present and powered on. Discovery receives nearby devices but has not
identified the pen; pen connection remains unconfirmed.
The original wireless charger is CPS8601 on I2C hub 3 at 0x41, with a separate
PMIC-Glink HBOOST dependency. Charging is not implemented by this stage.
Module compilation, host event/PM/diagnostic tests and final-image checks pass.
Stage4 boot and touch/resume are confirmed; pen input remains unconfirmed.
A fresh Wi-Fi regression result has not been separately reported for stage4.

The first pen helper looked in the removed `/sys/class/i2c-adapter` class and
reported zero hub-3 adapters. The working image already enables that hub and
its parent. The updated helper uses `/sys/bus/i2c/devices/i2c-N`, with tests
covering the current layout and OF-node fallback. It can be run directly on
stage4 without reflashing. CPS chip identification remains unconfirmed.
After the path correction, the user reported errno 6 (`ENXIO`) from the CPS
read: the adapter is found but the transaction receives no acknowledgement.
The user then reported GPIO10/12/15/85/111 all input/low/pulldown. Stage6 added
`caihong_pen_power.ko` and its boot-time registration, but the user reported a
black screen from power-on with no visible kernel log. Stage6 is withdrawn.
No SSH/IP observation or boot log has established whether this was an init
hang, display failure or another fault. Stage6a omits the CPS module/DT additions and restores the exact Stage5
init and DTB while retaining the corrected pogo module. Only pogo and the
passive helper differ from Stage5's archive records. The user confirmed
Stage6a boots without the black screen; the precise failure inside the removed
integration is still unknown. At that checkpoint CPS chip identification,
charging and pen input remained unconfirmed. Stage7a below now confirms chip
identification through manual diagnostics after boot.
See [CPS8601 bring-up](cps8601-bringup.md).
The user has now resumed pen-test preparation and deferred the minor pogo
pause. Start from the working Stage6b image and its existing Stage4 touch
module. The user confirms the pen has power, can charge under another system,
and stock shows a connection when it is attached magnetically. Source review
confirms CPS8601 attachment/ASK-address/uevent handling and the touchscreen's
separate `pencil_connected` → type selection → scan command path. The Android
component linking these to pairing/UI is not present in the inspected kernel
sources. `scripts/caihong-pen-scan.py --sweep` now tests the five documented
NT36532E modes on the existing image and keeps a candidate only after fresh
moving coordinates and tip pressure are observed. The summary of
`pen-scan-20260923-223638.json` now confirms `ack=observed` in modes 1–5,
with zero increments in IRQs, reads, touch frames, SPI errors, boot events,
pen packets, reports and checksum errors in every observation window.
Each mode reports `no_new_event_reads`; `candidate=None` and
`restored_mode=0`. This narrows the next investigation to pen wake/connection
and controller scanning, before coordinate decoding. ACK alone does not
establish that the controller detected a pen.

The supplied `bluetoothctl show` confirms a present, powered, pairable
controller supporting central and peripheral roles; it was not discovering
at the time. Its `Discoverable: no` state does not prevent discovery as a
central. A subsequent 25-second discovery returned `Discovery started`,
`Discovering: yes`, and multiple nearby devices including BLE advertisements.
The pen's known address was absent and `info` returned unavailable. Anonymous
addresses in the scan remain unidentified, so this does not prove the pen
never advertises or establish a mandatory Bluetooth connection for pen input.
The next investigation is the missing CPS8601 attachment path. If a
connection/wake change succeeds, rerun the existing
scan helper: the last sweep restored **0 (pen scan disabled)** and this driver
does not automatically select a scan type from BlueZ state. Host helper tests
pass; the withdrawn CPS boot integration is not reinstated.

Stage7 refactors `caihong_pen_power.ko` for manual loading on the existing
Stage6b device tree. Explicit `stage=1` registers only a real PMIC-Glink child
and client; it sends no messages and requests no GPIOs or I2C resources.
`stage=2` additionally reserves hub-3/address 0x41 and acquires the five known
GPIOs with charging inhibited and supply off. A separate `probe_once` write
is required for the bounded power/ID experiment. Setup and power phases have
kernel log markers; synchronous probe failures propagate to `insmod` and
unwind registration. No module alias, boot hook or DT addition is provided.
W=1 compilation, checkpatch, power/ACK tests and registration failure tests
pass locally. The user then authorized direct SSH testing on the tablet.
Stage 1 registered successfully with `transport_up=1`. Stage 2 failed cleanly
at IRQ bias configuration with `-524` (`ENOTSUPP`), before any HBOOST request
or CPS register access: this kernel's Qualcomm GPIO chip lacks `.set_config`.

Stage7a (module version `7.1`) uses a named pinctrl group configuration for
GPIO12's pull-up. The same working image then passed stage-2 setup and the
single power/ID request in 2.611 seconds: `phase=done result=0 cleanup=0
valid=0xff poisoned=0`, chip `0x8601`, firmware `0x0118`, mode `0x2`, IRQ
snapshot `0x3d`, VIN 5805 mV, IIN raw 125, temperature raw 25 and EPT 0.
These snapshots do not establish fresh attachment/ASK events or charging.
No CPS firmware or TX configuration was written. End state was charge inhibit
high and supply/wake/scan low, with GPIO12 input/high/pull-up. Both HBOOST
requests were acknowledged, and the module unloaded successfully after saving
the result. Wi-Fi remained up with carrier and SSH; touch IRQ/frame/contact
counters advanced without new SPI/checksum/start errors. Pen scan remains 0
and no pen coordinates are confirmed. No image change or reboot was needed.
Stage8 below extends this to attachment/ASK-address handling; the original boot
regression still has no proven cause. Reboot before another power experiment
under the diagnostic's existing one-attempt policy.

Stage8 adds root-triggered, bounded attachment diagnostics with stock protection
write/readback checks, fresh IRQ/ASK processing and checksum-validated identity
decoding. A separate workqueue cutoff and the observation loop limit the active
window to 15 seconds. No boot-image or Wi-Fi/touch payload change is involved.
The pen remained magnetically attached during the tests. GPIO-only operation
passed setup but timed out without fresh IRQ/ASK events. On a separate boot,
Stage8a's explicit vendor ENTER_TX_MODE command produced readback `0x2`, raw
mode `0x2` and one new interrupt with undefined flags `0x800`; I2C then stopped
acknowledging, ending the test with `-ENXIO` after 3.096 seconds including wake.
Both tests cleaned up with inhibit high, supply/wake/scan low, minimum HBOOST
acknowledged and no transport poison. The pen's known Bluetooth address stayed
unavailable. Wi-Fi/SSH remained usable. Touch counters advanced during Stage8;
Stage8a introduced no new errors but had no finger activity to validate input.
Reboot resets `pen_scan` to the unknown firmware default (`-1`); the earlier
sweep's mode 0 does not persist across boot.

Stage8b adds preservation of the initial ASK mailbox and a separately requested
vendor final 50 ms off/on supply cycle, gated by exact stock power-on protection
defaults and post-cycle readbacks. No timeout or failed command automatically
retries with this path. W=1 build, checkpatch and 37 attachment fault cases pass,
alongside existing power/ACK and registration tests. See
[`cps8601-attachment.md`](cps8601-attachment.md) for the experiment boundaries
and hardware results. On hardware, default protection values were
800/4000/9000/2200, so Stage8b correctly blocked the cycle before allowing
charging (`enabled=0 cycled=0 result=-95 cleanup=0`). Six startup IRQs occurred,
and the initial mailbox contained a combined address packet decoding exactly
to the user's known pen address. No checksum frame was captured, so this is
partial identity evidence, not a completed validated exchange. The next
receiver change should capture startup packets before the 2.5-second wait and
flag clearing lose them. Pen input and wireless charging remain unconfirmed.

After this startup-address evidence, the user removed the pen and drew during
a remote 10-second-per-mode sweep. All five commands were acknowledged; only
mode 4 had new event traffic (11 IRQs/reads/touch frames), with zero pen
coordinate reports in every mode. No candidate was found and mode 0 was
restored. The user then paused physical tests. The diagnostic module is
unloaded and its supply was disabled; Wi-Fi/SSH remains working, and touch
IRQ/frame counts advanced to 1092 without new SPI/checksum/start errors.
Resume by implementing early startup reception and capturing both checksum
and address frames with the pen attached, before another coordinate sweep.

Testing resumed on 2026-09-24. Stage8c's early receiver allowed 250 ms for ID
readiness, received only NACKs and exited cleanly without writes. Stage8d
extends readiness across the existing 2.5-second wake window and services ASK
as soon as the verified chip/firmware is ready. It keeps inhibit high and only
writes IRQ enable/clear registers. On a separate boot, the full exchange passed
in 1.004 seconds: `result=0 cleanup=0 poisoned=0`, six IRQs, seven flag
snapshots, two ASK packets, one checksum frame, one address frame and
`mac_valid=1`. The validated address matches the user's OPN2402. Last telemetry
was VIN 5817, IIN 155, temperature 25 and EPT 0. Supply/wake/scan ended low and
inhibit high; the module unloaded. Stage8d's first ID read succeeded, so the
earlier NACK cause remains unresolved. Both frames arrived within the first
second, explaining why the old 2.5-second delayed observation missed them.

Bluetooth discovery during the exchange still did not show the known pen.
Targeted LE Public and LE Random pairing requests did not connect and were
cancelled; their final Disconnected status was caused by local cancellation.
These attempts were made while the pen was attached. W=1 build,
checkpatch, 55 attachment/startup cases and the existing registration/power
checks pass, including common power cleanup on startup success/failure.

After detachment, the HCI capture did receive a connectable advertisement from
the verified address: **OnePlus Pencil Pro**, Digital Pen appearance, flags
`0x04`. Ordinary BlueZ discovery omitted this non-discoverable advertisement.
An address Pattern filter exposed it, after which connection, service
resolution and pairing/bonding succeeded. Battery initially read 94%; GATT
device information reports Maxeye and firmware `4D45.03.00.10` (2025-02-08).
No firmware update or vendor GATT command was used.

The connected/bonded pen then passed the existing mode-1 scan test: 171 fresh
event reads, 170 valid coordinate/pressure reports, no new SPI/pen checksum
errors. Mode 1 is retained. Evtest confirmed hover, contact, pressure-zero
release and proximity exit (13 complete enter/leave and down/up pairs), and
the user confirmed desktop pen taps. The stock Havon label for mode 1 does not
override this measured result with a Maxeye-manufactured pen. Wi-Fi and touch
remain working; no boot image or touchscreen module was replaced. The CPS
diagnostic is unloaded with its supply off. Automatic Bluetooth reconnection,
mode restoration across boot, pen suspend/resume, tilt/buttons and automatic
wireless charging remain future tests/work. Reproduction steps are in
[`cps8601-attachment.md`](cps8601-attachment.md#bluetooth-discovery-and-working-pen-input).

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

The user subsequently confirmed keyboard use but found Esc ineffective in
Vim. `evtest` identifies it as `KEY_BACK`; search produced no event and the
screenshot key reports `KEY_SYSRQ`. Stage5 exposed raw scancodes and mapped
most of the row, but its vendor consumer-page assumptions missed search,
microphone, touchpad toggle and lock. Captured keyboard usages are respectively
`0x72`, `0x68`, `0x6b`/`0x6c`, and `0x73`. Stage6 uses those actual usages, keeps
keyboard Fn held across media reports and corrects volume down/up to F11/F12.
The user confirmed the corrected mappings on the bootable Stage6a image.
Plain F4 still switches touchpad state inside the keyboard MCU. Stage6b sends
bounded asynchronous restore commands on both physical key edges, preserving
the requested hardware state (enabled by default, or explicit sysfs setting).
Fn+F4 remains a desktop `KEY_TOUCHPAD_TOGGLE` and does not change that hardware
target. Host tests cover both MCU usages, disabled targets, pending sysfs
updates, TX failures and removal; module compilation passes. The user reports
a brief pause with Stage6b and clarifies that all keys cause a pause. The
pause persists with disable-while-typing off and while evtest exclusively
grabs the keyboard. Desktop handling of these key events is therefore unlikely
to explain it. Raw touch-event gaps, contact releases and transport error
counters still need a hardware capture; MCU suppression and receive-path
problems remain hypotheses. The standalone `scripts/caihong-pogo-capture.py`
collects these on the existing image. Pause-free behavior and the full
restoration matrix are not confirmed.
The user considers this brief all-key pause a minor issue and explicitly
deferred further investigation in favor of pen testing. Keep it as an open
known issue; no timing capture result has been supplied and the cause is
unconfirmed. The collector is retained for a future investigation, not a
required step before pen work.
See [pogo keymap](pogo-keymap.md) for mapping and reproduction commands.
Stage6a preserves the exact tested stage4 touch module and v9 Wi-Fi payload;
its init and DTB match Stage5 byte for byte. Stage6b changes only the pogo
module relative to the bootable Stage6a image.

## Restart criteria

SC8547 work should resume only when there is a way to distinguish primary
current from an ADC/status fault (for example independent input-current
measurement or a validated register/fault snapshot at the excursion). Until
then, further controller tuning risks adapting policy to an unidentified
measurement failure.
