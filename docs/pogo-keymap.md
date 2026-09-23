# Caihong pogo keyboard: desktop function row

Stage6b F4 touchpad-state correction (brief pauses recorded; diagnosis deferred):

```text
mainline-boot-v2-stage6b-pogo-f4-wifi-v9.img
sha256: af13c4147d9201cf277e338d6072d646a1acf6609ab2594b4fb9f636e9bb3285
```

Stage6 was withdrawn after the user reported a black screen from power-on
with no visible kernel log. Stage6a removed the CPS8601 module and its DT additions, and restored the **exact
Stage5 init and DTB bytes**. It kept the corrected Stage6 pogo module. Only
the pogo module and passive status helper differ from Stage5's CPIO records.
The user confirmed Stage6a boots normally and the key mappings work, but
plain F4 still enables/disables the touchpad. Stage6b replaces only the pogo
module in that confirmed image. The specific Stage6 boot failure remains
unidentified from logs.

The user confirmed `KEY_BACK` for Esc and `KEY_SYSRQ` for screenshot.
Stage5's vendor consumer-page assumptions did not match this keyboard:
actual `MSC_SCAN` captures are microphone `70068`, touchpad `7006b`/`7006c`,
lock `70073` and search `70072`. These all use the **keyboard page**.
Stage6 maps these observed usages and corrects volume down to F11 and volume
up to F12. The user reported Konsole reacting to the previously swapped
volume-up/F11 mapping; use `evtest --grab` to isolate desktop shortcuts.

Search now updates Fn from the entire keyboard snapshot before translating
any slots. Its state remains active across separate consumer/media reports.
Holding search before a row key selects the media/system action; emitting
`KEY_FN` alone would not establish a desktop modifier. Esc maps to `KEY_ESC`
and Delete keeps its normal mapping.

| Physical key, left to right | Default | With search/Fn held | Wire usage |
| --- | --- | --- | --- |
| Brightness down | F1 | `KEY_BRIGHTNESSDOWN` | consumer 0x0070 |
| Brightness up | F2 | `KEY_BRIGHTNESSUP` | consumer 0x006f |
| Microphone icon | F3 | `KEY_MICMUTE` | keyboard 0x68 |
| Touchpad toggle (either MCU state) | F4 | `KEY_TOUCHPAD_TOGGLE` | keyboard 0x6b / 0x6c |
| Screenshot | F5 | `KEY_SYSRQ` | keyboard 0x46 |
| Lock | F6 | `KEY_SCREENLOCK` | keyboard 0x73 |
| Previous track | F7 | `KEY_PREVIOUSSONG` | consumer 0x00b6 |
| Play/pause | F8 | `KEY_PLAYPAUSE` | consumer 0x00cd |
| Next track | F9 | `KEY_NEXTSONG` | consumer 0x00b5 |
| Mute | F10 | `KEY_MUTE` | consumer 0x00e2 |
| Volume down | F11 | `KEY_VOLUMEDOWN` | consumer 0x00ea |
| Volume up | F12 | `KEY_VOLUMEUP` | consumer 0x00e9 |

Search uses keyboard usage `0x72` and emits `KEY_FN`. Scancodes use HID-style
page prefixes (`0x0c0000 | usage` for media, `0x070000 | usage` for keyboard).
`EV_MSC` accompanies the key event; it is not an error. Both touchpad usages
map to the same F4/Fn+touchpad-toggle behavior.

## F4 hardware side effect

The MCU changes its own touchpad state when the physical toggle key is
pressed, even when Linux translates that usage into F4. Stage6b queues the
existing stock `0x3a / 0x11 / 0x01 / disable` command to restore the host's
requested hardware state after either usage (`0x6b`/`0x6c`) changes on press
or release. It performs at most three writes, starting after 20 ms and spaced
by 20 ms. Unchanged reports and slot reordering do not restart the work.
Commands run in a worker after RX releases its mutex. TX and target changes
use that same mutex; teardown disables requeueing before closing serdev.

The default hardware target is enabled. An explicit `touchpad_enabled` sysfs
write updates the target on successful TX; subsequent F4 presses preserve
that choice, including an intentionally disabled touchpad. Startup setup also
uses the current target. A failed sysfs write does not replace the target.

Fn+F4 keeps emitting `KEY_TOUCHPAD_TOGGLE` for the desktop while restoring
the hardware target, so the MCU and desktop do not each apply a toggle.
Desktop-disabled touchpads stay disabled on plain F4. Fn+F4 still requires the
desktop binding; it is not a hardware toggle when used in a bare console.
This is correction after the firmware action, not a known command that
prevents that action; a brief interruption may remain and needs device testing.

The existing pogo `status` adds `touchpad_target_enabled`,
`touchpad_restore_left` and `touchpad_restore_error`. The last value reports
TX failure, not a verified MCU ACK; `touchpad_disabled` remains the last
observed state from keyboard status/heartbeat reports.

The output keycode is latched on press and retained until that physical usage
is released. Releasing Fn before the other key, pressing Fn after a held F
key, or moving a held key between report slots does not change its release
code or leave a key stuck. Fn uses a keyboard slot and does not consume either
media slot. Host tests exercise these cases, both touchpad states, captured
usages, Ctrl combinations, the 12-position row, ordinary Esc/Delete, repeated/duplicate reports, unknown
usages and truncated reports. Tests also inject MCU toggles at both edges,
preserve enabled/disabled targets, interleave sysfs requests with queued work,
exercise bounded TX failures and block requeueing during removal. The user
reports a brief pause with Stage6b and clarified that all keys cause a pause.
Pause-free F4 behavior and the full restoration test matrix remain unverified.

```sh
python3 external/out_tree_module-sc8547/scripts/test-pogo-keys.py
```

Use `sudo evtest --grab /dev/input/eventN` on **OnePlus Pogo Keyboard** to check
Esc, search, all 12 positions, then search+F1/F2/F3/F4/F5/F6/F11/F12. End the
grab with Ctrl+C. Test both release orders. Also check typing and
touchpad use after replacing the module. Desktop actions for the Fn layer
depend on desktop bindings and available hardware; emitting mic-mute, for
example, does not add microphone support.

For Stage6b, move the pointer while pressing plain F4 repeatedly, then test
Fn+F4 twice in the desktop. Also test plain F4 while the desktop has disabled
the touchpad, Alt+F4, normal typing and touchpad clicks. If motion stops,
record the pogo `status` after releasing the key and allowing the worker to
finish; persistent disable and a brief MCU interruption are different results.

## Brief pauses while typing

**Known issue, investigation deferred at the user's request.** The user
considers the pause minor and is resuming pen work. No raw timing capture has
been supplied. The diagnostic below is retained for later; it is not a
prerequisite for testing the pen.

The user reports that every key briefly pauses touchpad motion, even after
turning off **Disable while typing** and then testing with the keyboard
exclusively grabbed by `sudo timeout 30s evtest --grab`. This makes desktop
handling of the key events unlikely as the cause. Upstream libinput also
excludes F-keys from its typing filter; see
[`tp_key_ignore_for_dwt` and `tp_keyboard_event`](https://gitlab.freedesktop.org/libinput/libinput/-/blob/main/src/evdev-mt-touchpad.c).

Code review finds no ordinary-key delay or touchpad-disable command: the
restore worker is queued only by changes in the two physical F4 usages.
The key decoder does not release touch contacts. MCU suppression, serial
receive loss and touchpad event handling still need to be distinguished using
actual raw event timing. The stock driver exposes touchpad enable and gesture
commands, but no documented switch disabling suppression during key activity
was found; do not treat an unrelated gesture command as that switch.

Run the standalone collector on the tablet with the current Stage6b image:

```sh
sudo python3 scripts/caihong-pogo-capture.py
```

It requires only Python 3, automatically finds **OnePlus Pogo Keyboard** and
**OnePlus Pogo Touchpad**, and temporarily grabs only the keyboard. Keep moving
a finger in circles in the touchpad's center throughout the test: 4 seconds
without keys, then 7 seconds each pressing A, F1 and F4 about every 2 seconds
as prompted. After about 25 seconds the keyboard fd closes, releasing the
grab; error exits also close it. The touchpad stays available to the desktop.

The collector uses monotonic evdev timestamps for both devices, retains only
A/F1/F4 keyboard events, and records raw touchpad events. It reads existing
pogo counters before and after capture and omits raw identity bytes. It sends
no UART commands and changes no driver, GPIO or power settings. The current
driver's normal F4 correction still runs when F4 is pressed.

Provide the printed summary and, if needed, the generated
`pogo-capture-*.json`. The summary reports motion-event gaps near key press
and release, touch-contact releases, and changes in RX/TX/error counters.
A missing or ambiguous device, unavailable status node, evdev overflow and
counter reset are reported explicitly. Keep the raw JSON when there is an
error; it contains the partial capture. No kernel rebuild or new image is
needed for this test.

These are raw event intervals, not measured pointer pauses: holding a finger
still, lifting it, or reaching the pad edge can also create gaps. Continuous
raw motion during a visible pause points above evdev; a raw motion gap narrows
the investigation to the MCU/transport/driver path but does not alone identify
which one. Counter differences cover the entire run, including F4 traffic,
and cannot by themselves attribute an error to a particular key.

Host validation uses synthetic timing gaps, contact releases, queue overflow,
counter resets, native event decoding, device discovery, fd cleanup on errors
and a simulated 25-second capture:

```sh
python3 -B scripts/test-caihong-pogo-capture.py
```

## Build the test image

Keep the already-tested stage4 touch module. Compile only the pogo module:

```sh
mkdir -p build/nt36532e-stage6b/module/pogo
cp external/out_tree_module-sc8547/pogo/oneplus_pogo.c \
    build/nt36532e-stage6b/module/pogo/
cat > build/nt36532e-stage6b/module/Makefile <<'EOF'
obj-m += oneplus_pogo.o
oneplus_pogo-y := pogo/oneplus_pogo.o
EOF
make -C linux/out M="$PWD/build/nt36532e-stage6b/module" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

python3 external/out_tree_module-sc8547/scripts/build-nt36532e-test.py \
    --baseline mainline-boot-v2-wifi-deferred-hmt1-v9-official-bdf.img \
    --module build/nt36532e-stage4/module/nt36532e_ts.ko \
    --pogo-module build/nt36532e-stage6b/module/oneplus_pogo.ko \
    --firmware firmware-assets/novatek/DT-novatek-nt36532.bin \
    --output mainline-boot-v2-stage6b-pogo-f4-wifi-v9.img
```

The optional `--pogo-module` allows replacement of exactly
`lib/modules/oneplus_pogo.ko`, in addition to the existing init hook/touch
payload. The builder checks the module's name, vermagic and dependencies,
records the old/new module hashes, and verifies all remaining original archive
records byte for byte. Stage6b uses the exact stage4 touch module and preserves
the v9 kernel outside initramfs, WLAN modules/firmware and WLAN init commands.
Its DTB/init and every CPIO record except `oneplus_pogo.ko` match Stage6a.
`--pen-power-module` is currently rejected following the Stage6 boot regression.
The CPS8601 boot hook has been removed. See [CPS8601 investigation](cps8601-bringup.md).
