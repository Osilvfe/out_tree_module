# Caihong pogo keyboard: desktop function row

Stage6 test image (hardware validation pending):

```text
mainline-boot-v2-stage6-pogo-pen-power-wifi-v9.img
sha256: 06c3aadddd45ddacd9c7c4060697dc10ac53c9f367c2394711ef593d74b6e6bd
```

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

The output keycode is latched on press and retained until that physical usage
is released. Releasing Fn before the other key, pressing Fn after a held F
key, or moving a held key between report slots does not change its release
code or leave a key stuck. Fn uses a keyboard slot and does not consume either
media slot. Host tests exercise these cases, both touchpad states, captured
usages, Ctrl combinations,
the 12-position row, ordinary Esc/Delete, repeated/duplicate reports, unknown
usages and truncated reports. They do not establish physical-key behavior.

```sh
python3 external/out_tree_module-sc8547/scripts/test-pogo-keys.py
```

Use `sudo evtest --grab /dev/input/eventN` on **OnePlus Pogo Keyboard** to check
Esc, search, all 12 positions, then search+F1/F2/F3/F4/F5/F6/F11/F12. End the
grab with Ctrl+C. Test both release orders. Also check typing and
touchpad use after replacing the module. Desktop actions for the Fn layer
depend on desktop bindings and available hardware; emitting mic-mute, for
example, does not add microphone support.

## Build the test image

Keep the already-tested stage4 touch module. Compile the pogo and pen
diagnostic modules:

```sh
mkdir -p build/nt36532e-stage6/module/{pogo,charging}
cp external/out_tree_module-sc8547/pogo/oneplus_pogo.c \
    build/nt36532e-stage6/module/pogo/
cp external/out_tree_module-sc8547/charging/caihong_pen_power.c \
    build/nt36532e-stage6/module/charging/
cat > build/nt36532e-stage6/module/Makefile <<'EOF'
obj-m += oneplus_pogo.o
oneplus_pogo-y := pogo/oneplus_pogo.o
obj-m += caihong_pen_power.o
caihong_pen_power-y := charging/caihong_pen_power.o
EOF
make -C linux/out M="$PWD/build/nt36532e-stage6/module" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

python3 external/out_tree_module-sc8547/scripts/build-nt36532e-test.py \
    --baseline mainline-boot-v2-wifi-deferred-hmt1-v9-official-bdf.img \
    --module build/nt36532e-stage4/module/nt36532e_ts.ko \
    --pogo-module build/nt36532e-stage6/module/oneplus_pogo.ko \
    --pen-power-module build/nt36532e-stage6/module/caihong_pen_power.ko \
    --firmware firmware-assets/novatek/DT-novatek-nt36532.bin \
    --output mainline-boot-v2-stage6-pogo-pen-power-wifi-v9.img
```

The optional `--pogo-module` allows replacement of exactly
`lib/modules/oneplus_pogo.ko`, in addition to the existing init hook/touch
payload. The builder checks the module's name, vermagic and dependencies,
records the old/new module hashes, and verifies all remaining original archive
records byte for byte. Stage6 uses the exact stage4 touch module and preserves
the v9 kernel outside initramfs, WLAN modules/firmware and WLAN init commands.
The optional `--pen-power-module` adds a dedicated PMIC-Glink DT child and a
hub-3 phandle, plus a manual power/ID diagnostic module. Its init hook holds
charging inhibited and supply off; a test requires an explicit command.
See [CPS8601 investigation and test](cps8601-bringup.md).
