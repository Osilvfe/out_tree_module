# CPS8601 bounded attachment experiment

The Stage7a manual power/ID test established CPS8601 ID `0x8601`, firmware
`0x0118`, HBOOST control and I2C access on the working Stage6b image. Stage8
adds a short attachment/ASK experiment; it is not an automatic pen charger.
No boot image, Wi-Fi payload, touchscreen module or device tree is changed.

The pen must be magnetically attached during the experiment. After any power
attempt, reboot before another attempt. Stage 2 only reserves resources when
loaded; a separate root-only sysfs request enables the experiment:

```sh
insmod /tmp/caihong_pen_power-stage8b.ko stage=2
cat /sys/bus/platform/devices/caihong-pen-power/status
# GPIO-controlled automatic operation:
printf '1\n' > /sys/bus/platform/devices/caihong-pen-power/attach_once
# On a separate boot only, explicit stock ENTER_TX_MODE experiment instead:
# printf 'tx\n' > /sys/bus/platform/devices/caihong-pen-power/attach_once
# On another fresh boot, guarded stock final supply-cycle experiment instead:
# printf 'cycle\n' > /sys/bus/platform/devices/caihong-pen-power/attach_once
cat /sys/bus/platform/devices/caihong-pen-power/status
cat /sys/bus/platform/devices/caihong-pen-power/attach_status
```

The module keeps the one-attempt latch shared with `probe_once`. Firmware must
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
registration cleanup tests and 37 attachment fault cases. These cover checked
protection writes, fresh identity validation and ordering, invalid/partial
addresses, new-attachment reset, current/voltage/temperature limits, transport
loss, deadline cutoff, explicit TX command gating, startup-mailbox isolation,
supply-cycle default/readback failures, cutoff during the off interval and
cleanup. They do not
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

Resume with the pen attached and a fresh boot before another powered test.
Implement reception during the initial wake interval, capturing both checksum
and address frames within one startup epoch. Keep the existing protection and
cleanup checks; do not use the unmatched initial address as an authenticated
identity or bypass the supply-cycle default gate. Early reception is the next
implementation step, not functionality already present in Stage8b.

## Tested module artifacts

All three modules target `7.2.0-00012-gb35f5cb0b661-dirty`. Local build
directories retain the exact source, binary, verification manifest and private
hardware captures. The root-level copies now match the tested binaries:

| Artifact | SHA256 |
| --- | --- |
| `caihong_pen_power-stage8.ko` (GPIO-only test) | `25b62d7fe6894761cb6fdf827e82f2358833b5ee652de990af9011c320fa84d1` |
| `caihong_pen_power-stage8a.ko` (explicit TX) | `a84a11d1b630ab021ce7ce5597628250b70ff1d7c37a832f21a36fe7abaf9f2e` |
| `caihong_pen_power-stage8b.ko` (startup mailbox/default gate) | `ef5f0693147b8dece26590aee2f1293e7d890f26a983ae953201003adfccac45` |

These are manually loaded diagnostics, not boot-image updates or an automatic
wireless charging service.
