# Caihong CPS8601 pen charger

The user confirms OPN2402 has power, can charge under another system and shows
a connection in stock when magnetically attached. Linux Bluetooth discovery,
automatic attachment and pen input remain unconfirmed. The user has confirmed
stage4 touchscreen suspend/resume.
Keep that touch module and the v9 Wi-Fi payload unchanged while investigating.

## Resumed pen test preparation

The user has deferred the minor all-key touchpad pause and asked to resume
OPN2402 testing. The working image remains
`mainline-boot-v2-stage6b-pogo-f4-wifi-v9.img`; it already contains the tested
Stage4 touch module, pen input device and diagnostics. No new image is needed
for the current discovery step. For later input/controller snapshots, use:

```sh
sudo caihong-pen-status --skip-charger
```

This reads `touch_stats`, `pen_stats`, `pen_scan`, the Novatek event device
names and Bluetooth controller status. The known unpowered CPS NACK does not
need to be reproduced. The user's confirmation of a powered pen permits an
input-only scan experiment before restoring the CPS8601 attachment path.

Proceed according to the evidence:

| Check | Current evidence | Next step |
| --- | --- | --- |
| Pen power | User confirms it has power and charges under another system | Investigate discovery/wake; Linux wireless charging remains unconfirmed. |
| Linux Bluetooth | Controller present, powered, pairable; central/peripheral roles supported. No discovery result yet. | Run bounded discovery and query the pen device. A missing cached address alone says nothing about pen power. |
| NT36532E scan protocol | Modes 1–5 acknowledged, with zero IRQs/event reads in every window; OPN2402 mapping unknown | Check pen discovery/connection first; repeat the scan test after a wake/connection change. Last sweep restored mode 0. |
| Raw pen input | No hardware pen report confirmed | After a scan candidate is found, check hover, contact, pressure and leaving proximity in `evtest`. |

The supplied `bluetoothctl show` confirms `Powered: yes`, `Pairable: yes`,
`Discovering: no` and `Discoverable: no`. Being non-discoverable does not
prevent this controller from scanning or initiating a connection. Discovery
can be tested on the current image with:

```sh
sudo bluetoothctl --timeout 25 scan on
```

Keep the display awake; take the pen off the magnetic rail, move it and tap
with its tip during the scan. Record whether other nearby devices appear,
as controller power alone does not validate radio discovery. Then query
`bluetoothctl info <pen-address>`, using the pen address locally rather than
adding it to public logs or source. If the pen appears under another address,
retain its name, address type and advertised services for identification.
If discovered, inspect connection/service state and attempt a normal BlueZ
connection, pairing if required. If absent, do not infer a dead pen or a
specific touch protocol; attachment-driven wake remains unresolved.

Discovery/connection and touch-controller scan configuration are separate
checkpoints. Stock Android informs the touchscreen
of a pen type through `pencil_connected`; there is no equivalent automatic
BlueZ-to-NT36532E bridge in this driver. Bluetooth connection alone therefore
does not configure Linux pen scanning or establish valid coordinates. The
latest sweep restored **mode 0 (disabled)**. After a successful connection or
wake change, run the existing scan helper again before testing pen input.

If stock kernel logs are accessible while reconnecting the powered pen,
`nvt_notify_pencil_type` logs `value = ..., set pencil type to ...`. The latter
is the controller type after any DT mapping. Reading `pencil_connected`
returns only a connection boolean and **cannot recover the numeric type**.
The checked Caihong touch DTS has no `touchpanel,pen-id-map`, but that does not
identify OPN2402's type or prove that an installed stock image uses identical
board data. The generic source default of Havon is not a model identification.

Once power and scan setup are established, keep the display awake and run:

```sh
cat /sys/bus/spi/devices/spi0.0/pen_stats
sudo evtest
```

Select **Novatek NT36532E Pen**, then test hovering, drawing with varied
pressure, lifting the tip and moving out of range. End with Ctrl+C and read
`pen_stats` again. Expected reports include `ABS_X/Y`, `ABS_PRESSURE`,
`BTN_TOOL_PEN` and `BTN_TOUCH`; tilt and buttons need separate hardware checks.
Check that pressure/contact return to zero on lift and tool proximity clears
when leaving range. The `packets` counter includes no-pen/ID packets, so its
increase alone is not proof of pen detection: inspect `reports`, `format`,
`in_range`, coordinates and error counters together.

## Stock attachment and connection path

The inspected kernel sources establish these endpoints:

1. `cps_wls_tx_irq_handler()` handles `TX_INT_RX_ATTACH`, marks the pen near,
   adjusts HBOOST and starts attachment/charge monitoring. `TX_INT_SSP` can
   set `pen_present` before the BLE address has been validated.
2. `TX_INT_ASK_PKT` calls `cps_wls_get_ask_packet()`. Wireless packets with
   header `0x48` carry address-check bytes (`0xc1`) and two encoded address
   halves (`0xb6`/`0xb7`). After address validation the normal attachment path
   sends a uevent containing `pencil_status=1` and `pencil_addr=...`. Removal
   sends status 0. This is wireless-charger communication, not an HCI report
   that a Bluetooth connection has completed.
3. A userspace write to the touchscreen's `pencil_connected` proc entry passes
   a nonzero type to `notify_pencil_type()` and enables `MODE_PEN_SCAN`.
   `nvt_enable_pen_mode()` chooses the matching NT36532 extended scan command.
   The coordinates/pressure arrive over touchscreen SPI.

The Android userspace implementation between the charger uevent, Bluetooth
pairing and the visible connection popup is not included in the inspected
kernel tree. The popup alone therefore does not identify which of these
steps is complete. Similarly, CPS `tx_status=Connected` only reflects
`pen_present`; it does not query Bluetooth.

`notify_pen_state()` separately tracks magnetic attachment for touchscreen
power policy. While the screen is awake, it selects scanning according to
`is_pen_connected`; in supported screen-off gesture states it also checks
that the pen is not attached. The CPS8601 source inspected here does not call
this symbol, so do not infer a direct CPS-to-touch notifier from the older
P9418/RA9530 charger implementations.

On the current Linux image the CPS module is absent and no automatic service
translates charger/BlueZ state into `pen_scan`. Attaching a powered pen alone
therefore does not reproduce the stock path. The manual scan test exercises
the final touchscreen step without claiming to implement attachment,
Bluetooth pairing or pen-side wake/authentication. Whether this pen needs
additional connection/wake activity remains a hardware question.

The inspected Novatek `nvt_pen_control()` dispatches vibrator control and
feedback, not an identified connection/wake handshake. The CPS8601
`TX_INT_WAKEUP` handler signals a charger wake waitqueue; it does not establish
that the pen has been woken. No additional pen-side wake command is confirmed
by these paths.

## Powered-pen scan test

On the existing Stage6b image, take the charged pen off the magnetic rail,
keep the screen awake, and run:

```sh
sudo python3 scripts/caihong-pen-scan.py --sweep
```

Keep drawing short lines and tapping with the pen tip while each mode is
displayed. The test tries modes 1–5 for up to 8 seconds each, with a half-second
settle period per change. It stops at the first candidate with at least five
new valid-coordinate reports, three fresh sampled reports, two distinct
positions and observed tip pressure/contact. Empty/ID packets, unchanged
cached coordinates and hover alone do not qualify. These are candidate
criteria for this experiment, not a permanent retail-model mapping.

A candidate scan mode remains selected so `sudo evtest` can immediately test
**Novatek NT36532E Pen**. The existing driver also retains an acknowledged
selection across display/suspend cycles. Confirm raw hover, tip pressure,
release and leaving proximity before claiming a working pen or selecting a
default type in the driver. `--type N` tests just a stock-known type instead
of sweeping; `--seconds` accepts 3–15 seconds per mode.

The helper changes only the existing `pen_scan` attribute. It sends no CPS,
GPIO, HBOOST, firmware-flash or Bluetooth commands. Mode writes use the
driver's existing ACK polling. A command/read failure, restart, mode change,
SPI error or counter reset stops the sweep. If no candidate is found, or the
test fails/is interrupted, it attempts to restore the previous known mode.
If the initial mode was `-1`, the fallback is **0 (pen scan disabled)**;
the unknown firmware default cannot be reconstructed. Cleanup failure is
reported explicitly. The log uses exclusive creation and concurrent helper
runs are rejected with a lock.

Provide the printed result and the generated `pen-scan-*.json`. No candidate
does not mean the charged pen is dead: pen-side wake/connection, the protocol
selection or the touch event path may still be missing. The log includes
touch/pen counters and sampled raw coordinates for that next diagnosis.
Host tests cover candidate selection, false positives, timeout, cleanup,
interruption, sleeping/restarted controllers and counter resets.

The user supplied the summary of `pen-scan-20260923-223638.json` on
2026-09-23. All five modes show `ack=observed`, with zero increments in
`irq`, `reads`, `touch_frames`, `spi_errors`, `boot_events`, `pen_packets`,
`pen_reports` and `pen_checksum_errors`. Each has
`evidence=no_new_event_reads`; the sweep ended with `candidate=None` and
`restored_mode=0`, with no error or restore error printed. These are increments
during each observation window, not absolute lifetime counters.

This establishes that no new touch IRQ/event read occurred in those windows;
there was no new data for the pen decoder to process. It does not distinguish
pen wake/connection from controller scanning. The current command bytes, ACK
polling, 120-byte event payload and pen data offset 66 match the inspected
stock NT36532 path. No specific decoder change is justified by this result.

The original JSON already contains touch IRQ/read counters for every sample.
It can be summarized without another hardware test:

```sh
python3 scripts/caihong-pen-scan.py --summarize /path/to/pen-scan-TIMESTAMP.json
```

`--summarize` without a filename reads the most recently modified
`pen-scan-*.json` in the current directory and prints its path. It does not
require root, discover input devices, take the scan lock, write a file or
change scan mode. It works with logs from the first helper version. Missing
fields print as `?`, not zero; partial trials, command errors, counter resets
and changing controller state remain inconclusive.

The summary distinguishes no new event reads, reads that did not reach pen
dispatch, packets without valid coordinates, and valid-coordinate reports.
`ack=observed` reflects the driver's reported applied mode/zero command error
in the saved snapshot; it does not prove that the pen was awake or connected.
If an error aborted the run, the summary prints that error and the cleanup
result. No-IRQ/no-read results make pen wake/connection and controller scanning
the next checks; they do not identify which side failed. The supplied
`bluetoothctl show` establishes controller power; pen discovery is the next
hardware observation needed.
Offline tests cover these distinctions and prove the summary path avoids
device access and writes.

Full stock-like attachment still needs CPS support. Resume that as an
observable post-boot identification experiment: the previous Stage6 boot
regression remains undiagnosed and the withdrawn module is not ready to load
unchanged. First prove provider registration and the power/wake sequence with
chip ID 0x8601, then add protected wireless operation and address/attachment
events. Reading the chip ID alone neither charges nor connects the pen.

## Stage6 withdrawn; Stage6a boot recovery

The user reported a black screen from power-on after flashing Stage6, with
no visible kernel log at all. This does not locate the failure at module
insertion; the DT change or an earlier boot/display failure remain possible.
Wi-Fi uses a random MAC and the current IP was unknown, so SSH availability and Wi-Fi failure
have **not** been established. The old image synchronously inserted
`caihong_pen_power.ko` before `switch_root`. Even without triggering
`probe_once`, module registration creates devices/links and changes GPIO
configuration. The host ACK/sequence tests did not exercise actual device
registration or the tablet's boot path. No log currently proves a specific
lockup or display failure.

Stage6a removes that init hook, the module payload, `/pmic-glink/pen-power`
and the added hub-3 phandle. Its init and DTB are byte-identical to Stage5;
all archive records except the corrected pogo module and passive helper are
also identical. `--pen-power-module` now fails before reading/building an
image. The user confirmed Stage6a boots normally and its keyboard mappings
work; this narrows the regression to the removed integration without proving
a specific failure point. The source below remains experimental;
do not run the historical power-test commands until integration is diagnosed.

## Observed communication

The original helper used the removed `/sys/class/i2c-adapter` class. After
correcting discovery to `/sys/bus/i2c/devices`, the user obtained errno 6
(`ENXIO`) from the targeted read at hub 3/address 0x41. The GENI I2C driver's
error table maps a NACK to `-ENXIO` with the message "slv unresponsive, check
its power/reset-ln". This establishes adapter discovery and a failed transfer,
not a successful CPS chip ID read or proof of a failed/absent chip.

The updated `caihong-pen-status.py` also reads the existing main TLMM debugfs
snapshot for GPIO10/12/15/85/111. It selects the `f100000.pinctrl` bank, since
other GPIO banks can reuse local pin numbers. It does not request GPIO lines,
change direction, toggle outputs or measure HBOOST voltage. If debugfs is not
mounted, the helper reports that the snapshot is unavailable.

## Stock power dependencies

The board's `oplus-chg-23926.dtsi` uses I2C hub 3 (`i2c@98c000`), address
0x41, HBOOST default 5800 mV, and these TLMM pins:

| Pin | Stock role | Relevant behavior |
| --- | --- | --- |
| GPIO10 | Supply switch | High enables the supply path |
| GPIO12 | IRQ | Input, falling edge, pull-up when active |
| GPIO15 | Sleep/wake | High wakes the chip; stock waits up to 2500 ms |
| GPIO85 | Scan | Separate control; do not assume a level enables charging |
| GPIO111 | Off-state | High disallows charging; low allows it |

`oplus_cps8601.c:init_work_func()` first sets HBOOST, then drives the supply
switch high. After 10 ms it runs the stock firmware-update path, then raises
wake, waits, and checks chip ID. It next sets protection thresholds and IRQ
handling before normal charging. A mainline diagnostic should not copy the
automatic firmware-flashing or charging-enablement steps just to read an ID.

Stage5 lacked this sequencing. The user's GPIO snapshot shows all five pins
as input/low/pulldown, including GPIO10/15/111. This establishes the missing
GPIO control; it does not measure HBOOST voltage. The withdrawn Stage6 attempted a manual
bounded identification experiment; it has no successful hardware result.

## HBOOST protocol and integration

`oplus_wireless_pen_glink.c/.h` uses owner 32785, request/response type 1,
opcode 0x10007. The request consists of the 12-byte PMIC-Glink header, one
byte `reg_vout`, and three zero padding bytes. Stock computes the register
value as `(millivolts - 2000) / 50`, so the board's 5800 mV setting is 76.
The response is a 12-byte header and a 32-bit status; zero means success.
Stock waits 2500 ms. Its cleanup setting of 2000 mV is the minimum voltage
request, not evidence of a true regulator-off command.

This owner is independent of BATTMGR owner 32778 and must not modify the
paused SC8547 policy. Mainline `devm_pmic_glink_client_alloc()` takes the
transport from the client's parent device. An I2C client cannot be passed
directly as that device. A supported PMIC-Glink child/provider integration is
needed, including transport-down handling, serialized requests, exact response
validation and cleanup. A lost response must not be assumed successful.

## Source references

Paths relative to the stock tree `external/oneplus-sm8650-pad-pro`:

- `kernel_platform/qcom/proprietary/devicetree/oplus/oplus_chg/oplus-chg-23926.dtsi`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_cps8601.c`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_wireless_pen_glink.c`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_wireless_pen_glink.h`

Mainline references are `drivers/i2c/busses/i2c-qcom-geni.c`,
`drivers/pinctrl/qcom/pinctrl-msm.c` and `drivers/soc/qcom/pmic_glink.c`.

## Withdrawn Stage6 design (historical; do not run)

The module creates the dedicated `/pmic-glink/pen-power` DT child under the
actual, bound PMIC-Glink platform device. A managed supplier link orders probe,
suspend and removal against that provider. It also links to the GENI I2C
controller and reserves hub-3 address 0x41 using a dummy client. The packaging
script adds only this child and a unique hub-3 phandle, in addition to the
existing touch properties. Kernel code and SC8547 are unchanged.

At registration it requests GPIO111 high first, GPIO10/15/85 low, GPIO12 input
with pull-up. It sends **no HBOOST request at boot**. The historical test command was:

```sh
sudo caihong-pen-status --probe-power
```

The command sets 5800 mV through owner 32785 and waits for a validated ACK,
raises GPIO10, waits 10 ms, raises GPIO15, waits 2500 ms, then reads chip ID.
Only ID 0x8601 allows reads of firmware, mode, IRQ flags, VIN/IIN/temperature
and EPT. Register selectors are big-endian and values are little-endian,
matching stock's per-byte access. There are no CPS configuration, TX, IRQ-clear
or firmware writes. GPIO111 stays high for the entire test.

On completion or failure it lowers wake, scan and supply and requests the
minimum 2000 mV setting if the transport is still unambiguous. That setting is
not a regulator-off claim. The physical supply switch is disabled first.
A timeout, send failure or transport loss during the test latches `poisoned`:
no further request can consume a delayed response as its own ACK. Callbacks
never sleep; a transport-down event aborts waits. A mutex, wakeup source and
PM callbacks prevent a test from overlapping suspend/removal. One attempt is
accepted per module load, retained across driver unbind/rebind. Reboot before
another hardware experiment, especially after `poisoned=1`; module reload is
not a recovery procedure for an ambiguous firmware response.

The helper normally reads cached results without powering the chip or doing
raw I2C while the provider owns it. To read the result again:

```sh
sudo caihong-pen-status
cat /sys/bus/platform/devices/caihong-pen-power/status
```

`attempted=0 phase=idle result=-61` means no test has run. After a successful
identification expect `phase=done result=0 cleanup=0 valid=0xff chip_id=0x8601`,
with `charge_disable=1 supply=0 wake=0 scan=0`. `valid` bits 0 through 7
correspond to chip ID, firmware, mode, IRQ, VIN, IIN, temperature and EPT;
only set bits have meaningful latched values. `phase` identifies the failing
step. A read-phase result of `-6` still means NACK, now after acknowledged
HBOOST and GPIO wake sequencing. A `-110` with `phase=hboost` means ACK timeout,
so CPS power/wake was not enabled. Neither output pin readback nor an HBOOST
ACK proves physical rail voltage. Identification alone does not prove pen
charging, Bluetooth discovery or touch pen reports.

Software verification includes W=1 module builds, checkpatch and host tests
of the production ACK/power/I2C code with fake hardware: success, rejected
ACKs, malformed/unaligned replies, timeout and late reply, transport loss,
short transfer/NACK, mismatched ID, cleanup timeout and the invariant that
charging is always inhibited. These tests do not validate PMIC firmware or
physical GPIO behavior. See [image build instructions](pogo-keymap.md).
