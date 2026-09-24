# CPS8601 bounded attachment experiment

The Stage7a manual power/ID test established CPS8601 ID `0x8601`, firmware
`0x0118`, HBOOST control and I2C access on the working Stage6b image. Stage8
adds a short attachment/ASK experiment; it is not an automatic pen charger.
Stage8d now captures and validates both startup ASK frames on hardware.
Address-filtered Bluetooth discovery and pairing then enabled working pen
input on the existing touchscreen firmware. No boot image, Wi-Fi payload,
touchscreen module or device tree is changed.

The pen must be magnetically attached during the experiment. After any power
attempt, reboot before another attempt. Stage 2 only reserves resources when
loaded; a separate root-only sysfs request enables the experiment:

```sh
insmod /tmp/caihong_pen_power-stage8d.ko stage=2
cat /sys/bus/platform/devices/caihong-pen-power/status
# Current startup receiver (keeps charge inhibit high):
printf 'startup\n' > /sys/bus/platform/devices/caihong-pen-power/attach_once
# Separate boot only, older GPIO-controlled automatic-operation experiment:
# printf '1\n' > /sys/bus/platform/devices/caihong-pen-power/attach_once
# On a separate boot only, explicit stock ENTER_TX_MODE experiment instead:
# printf 'tx\n' > /sys/bus/platform/devices/caihong-pen-power/attach_once
# On another fresh boot, guarded stock final supply-cycle experiment instead:
# printf 'cycle\n' > /sys/bus/platform/devices/caihong-pen-power/attach_once
cat /sys/bus/platform/devices/caihong-pen-power/status
cat /sys/bus/platform/devices/caihong-pen-power/attach_status
```

The module keeps the one-attempt latch shared with `probe_once`. The `startup`
request is described below; the older `1`, `tx` and `cycle` experiments use
the following protection setup. Firmware must
match the identified `0x0118` before configuration writes are allowed. It
programs the Caihong stock thresholds and verifies each readback: OCP 500,
UVP 4000, OVP 12000 and FOD 400, plus the stock `0xffff` interrupt enable mask.
It checks voltage/current/temperature/EPT, records the old IRQ flags, clears
them, and requires a zero flags baseline before enabling the experiment.

Screen-on scan GPIO85 is set high, charge-inhibit GPIO111 low, and wake GPIO15
low, matching the stock transition to automatic operation. This temporarily
permits wireless power. The active window is limited to 15 seconds. A separate
delayed work item on the unbound workqueue can disable the physical supply
without waiting for the I2C/experiment mutex. The observation loop also enforces
the deadline. IRQ edges wake the observer, with 20 ms polling as a fallback.
Telemetry is sampled at least every loop reaching the next 100 ms checkpoint;
I2C latency can extend sampling intervals. A software timeout is not a hard
real-time hardware cutoff.

The optional `tx` request additionally checks that the command register is idle,
then writes only bit 1 (`TX_CMD_ENTER_TX_MODE`) of register `0x000b` after the
same protection and GPIO checks. Command and mode readbacks are recorded as
raw evidence, not proof of pen detection. This command exists in the vendor
driver, but its usual startup path instead ends with a supply cycle. The two
experiments are kept distinct; neither silently follows a timeout with another
startup sequence.

The `cycle` request first reads the four protection thresholds before writing
any configuration and requires exact stock values (500/4000/12000/400 in
OCP/UVP/OVP/FOD order). Otherwise it exits with `-EOPNOTSUPP` before permitting
charging. After normal preparation, it reproduces the vendor final GPIO10
50 ms off/50 ms on cycle, with scan high and inhibit/wake low. The same
15-second cutoff covers this cycle. The power lock prevents re-enabling supply
after cutoff or transport loss. Following the 50 ms startup wait, ID, firmware
and all four thresholds must still match before observation continues; IRQ
enable is restored with readback. These checks reduce uncertainty about reset
defaults but do not prove protection retention throughout the physical cycle.

Before clearing initial flags, the driver saves any initial ASK mailbox in a
separate root-only snapshot. It is not fed into the fresh-address decoder.
This preserves evidence of activity during startup without mistaking a single
cached packet for a new, checksum-validated identity.

VIN outside 4000–6500, IIN at or above 500, temperature raw at or above 50,
nonzero EPT, fault/removal flags, I2C/transport failure, invalid address checksum
or timeout ends the run. Cleanup inhibits charging and cuts wake/scan/supply
before requesting minimum HBOOST. A lost/ambiguous HBOOST reply retains the
existing poison latch. There are no firmware, calibration, software-reset or
arbitrary register-write controls.

For ASK, the stock `0x48/0xc1` packet provides three XOR check bytes; a combined
`0x48/0xb6` plus `0x48/0xb7` packet provides the encoded address. The decoder
accepts either arrival order but needs both in the current attachment epoch.
New attachment/removal clears partial identity. Checksum mismatch and all-zero
or all-ones addresses are rejected. A valid address ends the experiment.
`0x28/0x17` is treated as the stock stop-charging instruction. Raw packets and
the address are exposed only in root-readable `attach_status`, never in kernel
log messages. Do not copy them to public hardware reports.

Start Bluetooth discovery alongside an actual attachment test to observe any
resulting advertisement. A validated charger address is not a completed
Bluetooth connection or touchscreen protocol selection. The existing
`pen_scan=0` must still be changed through the scan helper before coordinate
testing once wake/connection progresses.

## First hardware result

With the pen attached and after a fresh boot, Stage8 setup succeeded. All
protection readbacks passed and initial flags `0x3d` cleared successfully.
The GPIO-controlled window ran to its deadline with no new IRQs, flags or ASK
packets: `enabled=1 irq=0 events=0 asks=0`, `result=-110 cleanup=0 poisoned=0`.
VIN/IIN/temperature remained approximately 5817/115/25, with EPT 0. The loop
ended before the backup cutoff work ran (`cutoff=0`); physical supply/wake/scan
were low and inhibit high after cleanup. The pen's known Bluetooth address
remained unavailable. Touch counters continued advancing with no new errors.
This establishes a missing response after the tested startup sequence, not
which startup step or pen-side condition is responsible.

The remote Python text-buffered write surfaced `EALREADY` after the failed
operation, while the driver's cached result is `ETIMEDOUT`. Use a single
unbuffered `os.write()` for subsequent one-shot sysfs requests so a flush/close
retry cannot obscure the original errno. The driver's latch prevented a
second power attempt.

Local validation includes W=1 compilation, checkpatch, the power/ACK tests,
registration cleanup tests and 55 attachment/startup fault cases. These cover checked
protection writes, fresh identity validation and ordering, invalid/partial
addresses, new-attachment reset, current/voltage/temperature limits, transport
loss, deadline cutoff, explicit TX command gating, startup-mailbox isolation,
supply-cycle default/readback failures, cutoff during the off interval and
cleanup. Startup cases additionally cover both packet orders, readiness after
1.5 seconds, permanent NACK, non-retryable I2C errors, unknown ID/firmware,
IRQ-write failure, partial/invalid identities, transport loss and deadline
cleanup. Startup permits only IRQ enable/clear writes; tests also check that
the common power path cleans up startup success and failure. They do not
replace the hardware test or prove reliable wireless charging.

## Explicit TX command result

Stage8a (module version `8.1`) ran on a separate boot, with the pen still
attached. Command readback was `0x2`, and the immediate mode readback stayed
`0x2`. A new IRQ produced flags `0x800`, which the inspected vendor header
does not define. There were no ASK or address packets. About 330 ms after the
command, I2C access returned `-ENXIO`; the driver stopped immediately. Total
request time, including initial wake, was 3.096 seconds. Last telemetry was
VIN 5829, IIN 0, temperature 25 and EPT 0.

Cleanup completed with inhibit high, supply/wake/scan low, `cleanup=0` and
`poisoned=0`; the module unloaded. Wi-Fi retained carrier and SSH. Touch stayed
enabled with no new SPI/checksum/start errors, but there were no finger events
during this boot's observation, so that run alone does not retest touch input.
After reboot, `pen_scan=-1` reports the unknown firmware default, replacing the
previous boot's explicit mode 0. The known pen address remained unavailable.
Neither the undefined interrupt nor mode `0x2` establishes pen detection or
explains why I2C stopped acknowledging. The final vendor supply cycle remains
a separate experiment, not an automatic retry of the explicit command.

## Startup mailbox and supply-cycle gate result

Stage8b (module version `8.2`) was tested on another fresh boot, with the pen
still attached. The `cycle` request ended in `attach-prepare` with `-EOPNOTSUPP`
after 2.753 seconds: power-on OCP/UVP/OVP/FOD values were 800/4000/9000/2200,
not the requested 500/4000/12000/400. All four reads succeeded. As designed,
the driver did not permit charging (`enabled=0`), write TX configuration or
cycle supply (`cycled=0`); the actual vendor final reset sequence remains
untested. Cleanup succeeded and the module unloaded. Do not relax the gate
merely to force that experiment through.

Six IRQs occurred during initial power/wake. Initial flags were again `0x3d`,
but this time the mailbox was saved before clearing anything. It contained
the combined `0x48/0xb6` and `0x48/0xb7` address packet. Local decoding matched
the user's known OPN2402 address exactly. No matching `0x48/0xc1` checksum
frame was captured, so `mac_valid` correctly remains false; the initial
snapshot is not inserted into the fresh identity state. The raw packet and
actual address remain in private local captures only.

This is direct evidence of pen-related ASK data already present before the
late observation window. The earlier zero fresh-event results do not imply
that startup never received pen data. Waiting 2.5 seconds before reading and
then clearing flags can miss earlier handshake frames; capture both frames
during startup next. A new touchscreen scan test is justified by this actual
charger/pen communication evidence. Charging, complete address validation,
Bluetooth connection and pen coordinates are still separate unproven steps.

## Post-startup pen scan and pause checkpoint

The user removed the pen and drew while SSH ran the existing scan helper for
10 seconds per mode. Commands for modes 1–5 were acknowledged. Modes 1, 2, 3
and 5 had no new event reads. Mode 4 had 11 new IRQs, reads, touch frames and
pen packet slots, but zero pen coordinate reports. All five modes had zero
new SPI or pen-checksum errors. There was no candidate; mode 0 was restored.
The mode-4 touch traffic alone does not identify a working pen protocol.

The user then paused physical testing. Final SSH checks showed the diagnostic
module/device absent, Wi-Fi up with carrier, and touch IRQ/frame counts at
1092 with no new SPI/checksum/start errors. The single touch checksum error
was already present before testing. The working boot image and Stage4 touch
module hashes are unchanged. Do not repeat the same sweep without a new
wake/handshake result.

At that checkpoint, the next step was reception during the initial wake
interval, capturing both checksum and address frames within one startup epoch.
Stage8d below implements and validates that step. The unmatched Stage8b
snapshot is never promoted to a checksum-validated identity, and the
supply-cycle default gate remains unchanged.

## Stage8d: complete startup exchange

The user resumed testing on 2026-09-24 with the pen attached. The `startup`
request replaces the blind 2.5-second wake wait with early identification and
IRQ/ASK handling inside the same 2.5-second observation budget. Only initial
ID reads returning `-ENXIO` are retried, until the deadline; other I2C errors
stop the test. ID `0x8601` and firmware `0x0118` must match before IRQ writes.
The driver enables IRQs and processes the first pending flags, rather than
clearing a baseline without reading its packet. The observer checks IRQ
completion with a 5 ms polling fallback and retains both checksum/address
frames within the current startup epoch.

GPIO111 stays high; GPIO85 stays low. There is no permission-to-charge
transition, TX command, protection-threshold write or supply reset in this
request. The software cutoff covers readiness and packet reception. Telemetry
uses the existing experiment limits, and success, timeout or fault all lead
to physical power-off and minimum-HBOOST cleanup. Chip activity during this
initial supply/wake sequence does not establish an automatic charging policy.

An initial Stage8c test used only a 250 ms readiness window. It received 20
ID NACKs and stopped after 0.282 seconds with successful cleanup and no
configuration writes. Stage8d permits readiness throughout the existing wake
window and records its duration. On a separate boot, it completed in 1.004
seconds with `result=0 cleanup=0 poisoned=0 valid=0xff`:

- ID `0x8601`, firmware `0x0118`, initial raw mode `0x1`.
- Six IRQs, seven processed flag snapshots, two ASK packets, one checksum
  frame and one address frame; `invalid=0 mac_valid=1`.
- The checksum-validated address matches the user's known OPN2402 exactly.
- Last telemetry VIN 5817, IIN 155, temperature 25, EPT 0.
- End state inhibit high, supply/wake/scan low; the module unloaded cleanly.

The event log puts the checksum frame about 0.46 seconds after supply-on and
the address frame about 1.00 seconds after supply-on. This confirms that the
old delayed receiver missed startup exchange data. Stage8d's first ID read
already succeeded (`startup_ready_reads=1 startup_ready_ms=0`), so this run
does **not** prove a longer readiness delay caused the success or explain
Stage8c's NACKs. Keep the bounded NACK handling and record further occurrences.

Ordinary Bluetooth discovery alongside this exchange did not expose the known
pen address. While it remained attached, direct management pairing requests
as LE Public and LE Random did not establish a connection and were cancelled.
The cancellation's `Disconnected` result is a local outcome, not a pen
rejection. The subsequent detached capture and filtered discovery below
resolved this checkpoint. The existing NT36532E scan commands match the
inspected vendor table; no new command or touch firmware was needed.

## Bluetooth discovery and working pen input

After the user removed the pen and tapped the screen, ordinary discovery
again omitted it. HCI capture, however, received its exact verified public
address in a connectable/scannable legacy `ADV_IND`, with name **OnePlus Pencil
Pro** and appearance **Digital Pen (0x03c7)**. Its advertising flags were
`0x04` (BR/EDR not supported), without the LE discoverable flags. Ordinary
discovery received 79 other device entries in that run. Absence from the
`bluetoothctl` list therefore did not mean absence of pen advertisements.

An address `Pattern` discovery filter exposed the same device to BlueZ.
`Connect` completed GATT service resolution; subsequent pairing with a
NoInputNoOutput agent succeeded. Final state was Connected/Paired/Bonded and
ServicesResolved true. Battery was initially 94%, later 93%. Standard device
information reports manufacturer **Maxeye**, firmware
`4D45.03.00.10 09:48:54 Feb 8 2025` (spacing normalized). The Bluetooth HID
report map describes mouse/keyboard reports; touchscreen pen coordinates are
confirmed separately through the existing Novatek input device. Actual
addresses and raw captures remain private.

With the pen connected and bonded, the existing scan helper found **mode 1**
immediately: 171 IRQ/event reads and 170 new valid pen reports in its candidate
window, with moving coordinates and nonzero tip pressure. No new SPI or pen
checksum errors occurred. The helper stopped and retained mode 1. Although
the stock enum calls this mode Havon, the pen's manufacturer string does not
select the enum; use the observed result for this tested setup.

A subsequent evtest capture recorded 13 matching pen-proximity enter/leave
pairs, 13 tip-down/up pairs, 13 pressure-zero releases, 652 hover frames and
462 contact frames. Maximum observed pressure was 10584 within the configured
0–16383 range. The user confirmed working desktop pen taps. This establishes
coordinates, pressure, hover and release behavior. The user subsequently
confirmed pen sleep/resume works; tilt and buttons remain unvalidated.
Automatic connection and scan restoration later passed a full reboot with
the rootfs recovery service enabled. The native Wayland Krita test received positive
user feedback for basic pressure drawing.

No firmware was flashed or replaced. Keep the working Stage6b image, Stage4
touch module and mode 1. The CPS diagnostic is unloaded and its supply is off;
the Bluetooth bond is retained. Automatic wireless charging remains a separate
implementation task.

### Reproduce discovery and connection

Use the locally known or CPS-checksum-validated address; it is deliberately not
hardcoded in the repository. Take the powered pen off the magnetic rail and
move/tap it before discovery. Start an interactive client so the discovery
filter's D-Bus owner remains alive:

```sh
bluetoothctl --agent NoInputNoOutput
```

Within that client, replace `<pen-address>` with the actual address:

```text
menu scan
transport le
pattern <pen-address>
back
scan on
```

After the named device appears, run `connect <pen-address>`. On first setup,
run `pair <pen-address>` as well; an existing bond does not need re-pairing.
Use `info <pen-address>` to confirm connection and pairing, then `scan off`
and `quit`. This workflow does not require making the host discoverable or
modifying its address. No vendor GATT payload, firmware update or factory
reset command is sent.

For this already validated OPN2402 setup, enable the tested scan mode as root:

```sh
printf '1\n' > /sys/bus/spi/devices/spi0.0/pen_scan
cat /sys/bus/spi/devices/spi0.0/pen_scan
```

The driver retains the selection across its panel/touch restarts, but a whole
system reboot returns it to the unknown firmware default (`-1`). A separate
[rootfs recovery service](pen-autoconnect.md) now restores the configured mode
and reconnects the already paired pen. It is installed with its timer enabled;
scan restoration and disconnect/reconnect tests pass. A full reboot also
restored connection and scan mode automatically. This does not run the CPS diagnostic automatically.
Stop its timer before using the existing scan helper to validate a different
setup; a manufacturer label alone is not a protocol choice.

## Tested module artifacts

All listed modules target `7.2.0-00012-gb35f5cb0b661-dirty`. Local build
directories retain the exact source, binary, verification manifest and private
hardware captures. The root-level copies now match the tested binaries:

| Artifact | SHA256 |
| --- | --- |
| `caihong_pen_power-stage8.ko` (GPIO-only test) | `25b62d7fe6894761cb6fdf827e82f2358833b5ee652de990af9011c320fa84d1` |
| `caihong_pen_power-stage8a.ko` (explicit TX) | `a84a11d1b630ab021ce7ce5597628250b70ff1d7c37a832f21a36fe7abaf9f2e` |
| `caihong_pen_power-stage8b.ko` (startup mailbox/default gate) | `ef5f0693147b8dece26590aee2f1293e7d890f26a983ae953201003adfccac45` |
| `caihong_pen_power-stage8c.ko` (250 ms readiness experiment) | `d449a9f77ca0e70aa247a5aeb60b25d08efdf8bf498a74a70fcae2e5507dca71` |
| `caihong_pen_power-stage8d.ko` (complete startup exchange) | `66936cfc82d8379ee00054312aa57ecb0aeb3acbc496df7d8cff0b89e618b65c` |

These are manually loaded diagnostics, not boot-image updates or an automatic
wireless charging service.
