# Caihong NT36532E bring-up

Status: 2026-09-23. Stage3 desktop touch and a simple browser 10-point test
passed. Sleep/resume later failed with no `evtest` events and a reported
`nt36532e_resume` PM error of `-110`. Stage4 contains a panel-sequencing fix
and pen diagnostics; the user confirmed sleep/resume now works. The user now
confirms OPN2402 has power and charges under another system; Linux Bluetooth
connection, automatic attachment and pen input remain unverified. The first
five-mode sweep acknowledged every command but produced zero IRQs/event reads
and zero pen reports; it restored scan mode 0. Bluetooth is present and powered
on. Next check pen discovery on the existing image; see
[the recorded result and next steps](cps8601-bringup.md#resumed-pen-test-preparation).

## Stage1 packaging failure

The reported boot had no NT36532E module in `lsmod` and no Novatek probe logs.
Inspection of the actual boot image confirmed that its embedded `/init` did
not load the touch module. A load stanza had been added to a different init
template from the one consumed by the build.

Stage1 also consumed the default firmware directory, replacing the working
HMT1 AMSS/M3 and official BDF with different files and losing the v9 deferred
WLAN loading sequence. It must not be used as the Wi-Fi baseline.

Withdrawn image:

```text
mainline-boot-v2-nt36532e-stage1.img
sha256: 2bd4ff1cca3892414f265704e1a96e0f657865555b37f70f3ab74da5fe8b324a
```

## Stage3 event handling

The stage2 screenshot confirms NT36532E cascade detection, all 16 firmware
partitions loaded, both input devices registered, and `spi0.0` bound. The
user confirmed that the `spi0.0` interrupt count increases when touching and
that `evtest` reports codes 53/54 (MT coordinates), 58 (MT pressure), 330
(BTN_TOUCH), and 0/1/24 (single-touch coordinates/pressure). The reported
failure is in the graphical desktop, not merely lack of actions in a console.
This establishes the IRQ/SPI/input path; it does not establish correct
multitouch release or desktop handling.

The previous code called `input_mt_sync_frame()` without
`INPUT_MT_DROP_UNUSED` and never explicitly released a slot. Consequently,
lifted contacts remained active in the input core. Stage3 enables slot
release and lets the input core derive BTN_TOUCH consistently from the active
slots, including on suspend. The user subsequently confirmed normal desktop
touch and 10 simultaneous contacts in a browser test.

Stage3 also follows the vendor `nvt_get_fw_info_noflash()` layout selection:
FWINFO byte 13 equal to `0xf1` selects 16-bit coordinates, otherwise the
legacy 12-bit layout is used. It reads boot events around firmware-info
setup and IRQ enable, including after resume, and handles touch/pen checksum
results independently. Pen interaction is still unverified.

Read-only `touch_stats` and `last_event` attributes under the SPI device expose
IRQ/read/valid-frame/contact/error counts and the latest 121-byte event. Reads
of these attributes do not perform SPI transactions or consume new events.
Only the first three events and first three failures are printed in dmesg.

```text
mainline-boot-v2-nt36532e-stage3-events-wifi-v9.img
sha256: 8a1694c88ce131cd9848322ce7b134ab779fbdda00c6c05b7ae00a2c7b87f073
```

The retained stage3 screenshot reported:

```text
irq=129 reads=131 frames=129 contacts=114 spi_errors=0
checksum_errors=1 out_of_range=0 boot_events=0 last_error=0
enabled=1 fw=01 protocol=f1 high_res=1
```

The single checksum mismatch was during startup (`ff 00 00 00 00 ff ...`);
subsequent finger events worked. `contacts` is cumulative, not a simultaneous
finger count. This image remains the known desktop/10-point checkpoint, with
the later-discovered resume defect recorded rather than marked as a full pass.

## Stage4 panel resume and pen

```text
mainline-boot-v2-nt36532e-stage4-pen-resume-wifi-v9.img
sha256: 8a5d3ca88ef713e0a1518161cc35d2c39ba3bfd1a02420e0bfe9b9df0d125d7b
```

The old system-resume callback reset/uploaded the touch controller without
coordinating with the display. The mainline `panel-novatek-nt36532.c` resets
the panel and manages its VDDIO during prepare/unprepare; stock Caihong also
sets `lcd_trigger_load_tp_fw_support`. Reloading only on system resume misses
display-only power cycles and can run while the panel is still off or before
a later panel reset. The reported `-110` confirms a timeout, not by itself
which firmware/reset wait failed. The old callback returns before enabling
IRQ on any error, explaining the silent input device afterwards.

Stage4 follows the DT's existing `panel` reference through DRM panel-follower
callbacks. It stops IRQ/input before panel unprepare, then queues a reset,
firmware upload and event setup after preparation. The worker also waits for
SPI device resume so either callback order works. Firmware is requested once
at probe and retained until unbind, removing filesystem access from resume.
Failed startup remains visible in counters/logs and retries on the next
panel power cycle. The change does not add automatic unbounded retries.

The user subsequently confirmed normal sleep/resume with this stage4 image.
Retain it as the current working touch/resume checkpoint. Repeated long-term
power-cycle coverage and pen operation remain separate validation tasks.

Because startup now runs after panel preparation, module insertion, a bound
SPI device and input registration alone no longer establish controller startup.
Check `touch start ... complete`, `enabled=1` and increasing valid frames.
`starts` includes first startup; `start_failures`, `start_error`, `sleep_error`,
`stops`, `panel_ready` and `suspended` describe subsequent power transitions.
These sysfs reads remain passive.

Pen changes include an independent axis transform/range, pressure 0–16383
(previously clipped to the finger limit of 1000), physical axis resolution,
transformed tilt up to ±60 degrees, hover/exit/button handling, checksum and
range diagnostics. The touch packet decoder and 10-slot release behavior are
retained. Pen scan restore errors do not prevent finger touch from restarting.

The optional `pen_scan` sysfs interface accepts vendor protocol types:

| Value | Vendor name |
| --- | --- |
| -1 | Unconfigured/unknown; read-only state, firmware default at startup |
| 0 | Disable scanning |
| 1 | Havon |
| 2 | Maxeye |
| 3 | Maxeye 2nd |
| 4 | Sunwoda |
| 5 | Maxeye 3rd |

Writes use the vendor's acknowledged extended command and bounded polling.
An ACK timeout is reported as an error and leaves the scan state unknown.
There is no established mapping from retail model **OPN2402** to these types;
do not infer it from the Bluetooth address. No type is selected automatically.
The powered-pen helper `scripts/caihong-pen-scan.py --sweep` can now try those
five documented modes with bounded observation and explicit cleanup; see
[the powered-pen test](cps8601-bringup.md#powered-pen-scan-test). No retail model
mapping is assumed. Once a type is established, write it using, for example, `printf '%s\n'
"$TYPE" | sudo tee /sys/bus/spi/devices/spi0.0/pen_scan`. `pen_stats` shows
packet/report/error counters, raw values, scan state and command errors.

## Pen power and Bluetooth investigation

The stock source identifies CPS8601 at address 0x41 on I2C hub 3
(`i2c@98c000`), with IRQ GPIO12, sleep GPIO15, scan GPIO85, off-state GPIO111
and switch-enable GPIO10. Its supply uses HBOOST at a stock default of
5800 mV through PMIC-Glink owner 32785/opcode 0x10007. This is separate from
the BATTMGR owner used by the paused SC8547 work. The current mainline board
has no CPS8601 driver/node or HBOOST implementation. This stage does not
enable wireless pen charging or flash CPS firmware.

Relevant stock source, relative to `external/oneplus-sm8650-pad-pro/`:

- `kernel_platform/qcom/proprietary/devicetree/oplus/oplus_chg/oplus-chg-23926.dtsi`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_cps8601.c` and `.h`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_wireless_pen_glink.c`
- `vendor/oplus/kernel/touchpanel/oplus_touchscreen_v2/Novatek/NT36532_noflash/nvt_drivers_nt36532_noflash.c`
- `vendor/oplus/kernel/touchpanel/oplus_touchscreen_v2/touchpanel_proc.c`

Stock CPS8601 obtains the pen BLE address through the wireless protocol and
emits pencil status/address notifications. Android userspace then notifies
the touch framework of connection/type to enable the matching scan protocol.
The retail model mapping has not been found in these kernel sources.

Stage4 installs a Python 3 helper into `/usr/local/sbin/` on the rootfs:

```sh
sudo caihong-pen-status --address "$PEN_ADDRESS"
```

Supply the pen address locally; it is not stored in this repository. The
helper prints touch/pen stats, event device names, Bluetooth controller state
and cached pen information. It reads only known CPS status registers on the
identified hub, checks ID 0x8601 first, and skips raw reads if a kernel driver
owns that address. It does not scan arbitrary I2C addresses, change GPIOs,
power up the charger, clear IRQs, flash firmware, or pair/connect Bluetooth.
Use `--skip-charger` for input/Bluetooth only. An I2C failure leaves supply,
sleep state and access unresolved; it does not prove a dead pen or absent IC.

The helper embedded in the original stage4 image has a discovery bug: it looks
in `/sys/class/i2c-adapter`, which no longer exists in this kernel. The user
therefore saw `expected one hub-3 adapter ... found 0` before any CPS register
transaction. The hub and parent are both `okay` in the actual pinned DTB.
The current script finds numeric adapters under `/sys/bus/i2c/devices` instead
and lists their OF paths if the target is still missing. Download/run the
updated `scripts/caihong-pen-status.py` directly; no image change is required.
The old embedded helper is copied back into `/usr/local/sbin` at boot, so use
the separately downloaded script for this diagnosis after any reboot.

The supplied `bluetoothctl show` now confirms a present, powered controller.
Run `sudo bluetoothctl --timeout 25 scan on` and then query the pen address
with `bluetoothctl info`. `Discoverable: no` does not prevent scanning. The
helper itself only queries cached info. An undiscovered address
does not establish whether the pen is charged, advertising or connected.

## Preserved Wi-Fi baseline

The previous stage2 image is retained for comparison:

```text
mainline-boot-v2-nt36532e-stage2-wifi-v9.img
sha256: 467f86cbe34ff9da1ebb2f51b3553505f2ebcf5987485da66ea06dfdf0aaf49e
```

The pinned baseline is the image the user reported working well with Wi-Fi:

```text
mainline-boot-v2-wifi-deferred-hmt1-v9-official-bdf.img
sha256: 0dcedad3958e689331881d791bbfecafd7055905d628f9b1a776dbb8ad6b15ae
```

`scripts/build-nt36532e-test.py` refuses any other baseline. It performs these
limited changes:

- Add `nt36532e_ts.ko`, `novatek/DT-novatek-nt36532.bin` and the pen status
  helper to the embedded
  initramfs. Every existing record except `/init` is preserved byte for byte,
  including modes, ownership and links. Stage5 may additionally replace the
  pogo module via the explicit `--pogo-module` option, which records and checks
  that single extra replacement; see [pogo keymap](pogo-keymap.md).
- Append the touch startup hook after the original WLAN load and before the
  root transition. Removing this hook recovers the original `/init` exactly.
  The hook installs touch firmware into the Arch rootfs at
  `/usr/lib/firmware/novatek/` for later module reprobes, and installs the helper.
- Add GPIO161 active-low reset, `novatek,pen-support`, pen pressure/tilt limits
  and 177/250 mm raw-axis dimensions to the existing touch node. The sorted
  DT comparison permits only these six properties.

To fit the extra files without relinking the kernel, the builder gzip
compresses the new CPIO and pads the rest of the original initramfs region
with zeros. The v9 embedded configuration has `CONFIG_RD_GZIP=y`; Linux's
initramfs unpacker supports compressed CPIO with zero padding. The Image size,
all addresses and every byte outside that region are preserved. This also
preserves built-in WLAN firmware, not just firmware in the initramfs.
The builder keeps the boot-v2 metadata and command line, and regenerates and
checks its image ID. This is specific to the pinned raw, unsigned v9 image.

The image is 123625472 bytes, below the 192 MiB boot partition size. The
builder rereads the generated image, decompresses its initramfs and compares
all records against the intended archive before publishing it. A `.img.json`
manifest and `.img.sha256` file accompany the image. GNU `cpio` extraction and
shell syntax checks also passed; supplying stage1 as the baseline was rejected
before any output image was created. These checks do not establish hardware
success or rule out interaction between newly active touch hardware and Wi-Fi.

## Reproduce

From the Caihong project directory, with this repository checked out as
`external/out_tree_module-sc8547`, compile only the touch module against the
matching prepared kernel build:

```sh
mkdir -p build/nt36532e-stage4/module/touchscreen
cp external/out_tree_module-sc8547/touchscreen/nt36532e.c \
    build/nt36532e-stage4/module/touchscreen/
cat > build/nt36532e-stage4/module/Makefile <<'EOF'
obj-m += nt36532e_ts.o
nt36532e_ts-y := touchscreen/nt36532e.o
EOF
make -C linux/out M="$PWD/build/nt36532e-stage4/module" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

python3 external/out_tree_module-sc8547/scripts/build-nt36532e-test.py \
    --baseline mainline-boot-v2-wifi-deferred-hmt1-v9-official-bdf.img \
    --module build/nt36532e-stage4/module/nt36532e_ts.ko \
    --firmware firmware-assets/novatek/DT-novatek-nt36532.bin \
    --output mainline-boot-v2-nt36532e-stage4-pen-resume-wifi-v9.img
```

The builder refuses to overwrite an existing image. Requirements: Python 3,
`dtc`, `fdtget`, `fdtput`, `modinfo`, `mkbootimg`, and a compiled module.
The existing kernel configuration matches v9, with module versioning and
signatures disabled. The checked module vermagic is:

```text
7.2.0-00012-gb35f5cb0b661-dirty SMP preempt mod_unload aarch64
```

Vermagic is a compatibility guard, not a substitute for using the matching
kernel build. Do not rebuild/replace the baseline kernel as part of this test.
Changing the module build or debug paths may change the resulting image hash.
To reproduce historical stage2, use repository commit `770cb12` and its
documented module build directory instead. Stage3 is retained at `fc122e1`;
the original tested stage4 source/packager is retained at `6b3a0bb`.

Host packet checks run with:

```sh
python3 external/out_tree_module-sc8547/scripts/test-nt36532e-events.py
python3 external/out_tree_module-sc8547/scripts/test-nt36532e-pm.py
python3 external/out_tree_module-sc8547/scripts/test-caihong-pen-status.py
```

They compile the actual driver decoder/FWINFO functions with SPI/input sinks
and fixed wire packets, checking both coordinate formats, corrupt checksums,
pen pressure/hover/exit/tilt/buttons, scan ACK/timeout/errors, empty release
frames, boot notifications, bounds and SPI errors. PM tests exercise actual
callbacks with both panel/bus resume orders, blank/unblank, queued work,
IRQ balance and failed startup/pen/sleep commands. C tests passed with
undefined-behavior/bounds sanitizers. The helper test mocks I2C transfers and
checks selectors, endian order, short transfers and chip/driver guards.
These tests do not emulate kernel scheduling, the input core or real hardware.

The firmware was extracted from the stock Caihong `firmware-data-0` property
in `caihong-oplus-tp-23926_firmware.dtsi`. It is 249856 bytes with SHA256
`fc6f5124d7f571f1090731b77fff0dcaf0abacbc2249b0dfa4891578919b1788`.

## Device checks

After booting stage4, first confirm Wi-Fi still connects. Then collect:

```sh
sudo dmesg | grep -Ei 'caihong-touch|nt36532|novatek|ath12k'
lsmod | grep -E 'nt36532|ath12k'
cat /proc/bus/input/devices
cat /sys/bus/spi/devices/spi0.0/touch_stats
```

`caihong-touch: nt36532e_ts module inserted` means only driver registration
succeeded. `caihong-touch: device bound: spi...` plus Novatek input devices
shows that probe completed, but also check the asynchronous `touch start`
result and `touch_stats`. If there is no bound device, retain the associated
SPI/firmware error logs; do not infer success from `lsmod` alone. The hook
records both console output and kernel messages, including insertion errors.

Touch and lift a finger, then read `touch_stats` again. `frames` counts valid
touch packets, while `contacts` counts accepted contacts across those frames;
it is not the number of fingers currently down. `reads` includes startup
reads, so it may exceed `irq`. Error counters separate SPI failures, bad
checksums and coordinates outside the configured range. `last_event` retains
the last read packet for diagnosing persistent errors.

In `evtest`, select **Novatek NT36532E Touchscreen**. Finger down should create
an `ABS_MT_TRACKING_ID` (code 57), and finger up should emit tracking ID `-1`
and BTN_TOUCH (code 330) `0`. Stop evtest before testing desktop interaction.
If those events are correct but the desktop still does not react, run
`sudo libinput debug-events --device /dev/input/eventN` with the touchscreen's
actual event number. Look for `TOUCH_DOWN`, `TOUCH_MOTION`, `TOUCH_UP` and
`TOUCH_FRAME`; also inspect `udevadm info -q property -n /dev/input/eventN`
for `ID_INPUT_TOUCHSCREEN=1` and seat tags. Avoid a permanent calibration or
udev override until the event stream and classification are known.

For stage4, test display blank/unblank and system suspend/resume separately,
several times. After each wake, verify desktop tapping/10-point input and
Wi-Fi connection. Collect `touch_stats` and `dmesg | grep -Ei
'nt36532|touch start|PM:'` if touch fails. Expect `enabled=1`, `panel_ready=1`,
`suspended=0`, `start_error=0` after successful startup, and `starts` to increase
after a display power cycle. Pen needs separate powered/connected testing:
select **Novatek NT36532E Pen** in `evtest`, then check hover, tip pressure,
buttons, leaving proximity, and `pen_stats`.
