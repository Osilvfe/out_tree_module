# Caihong pogo keyboard: desktop function row

Stage5 test image (hardware validation pending):

```text
mainline-boot-v2-stage5-pogo-fn-pen-diag-wifi-v9.img
sha256: 7badfe48f8ac6404f61bfdc815c1f6f8a8c0b43d6dbf8b2d88c3bba0f20493a9
```

The user confirmed that the physical Esc key reports `KEY_BACK`, the search
key produces no event in the original driver, and the screenshot key reports
`KEY_SYSRQ`. The stock driver labels search as vendor consumer usage 0x0393;
it was missing from the mainline port's media table. Esc uses consumer AC Back
0x0224. Screenshot uses keyboard usage 0x46, so it needs a separate mapping
from the media keys.

Stage5 maps Esc to `KEY_ESC`, search to `KEY_FN`, and the 12 keys between Esc
and Delete to F1–F12 in the physical order provided by the user. Holding search
before pressing a function-row key selects the original media/system action.
This Fn behavior is implemented in the driver, since emitting `KEY_FN` alone
does not turn it into a desktop modifier. Delete keeps its normal mapping.

| Physical key, left to right | Default | With search/Fn held | Wire usage |
| --- | --- | --- | --- |
| Brightness down | F1 | `KEY_BRIGHTNESSDOWN` | consumer 0x0070 |
| Brightness up | F2 | `KEY_BRIGHTNESSUP` | consumer 0x006f |
| Microphone icon | F3 | `KEY_MICMUTE` | vendor consumer 0x0391 |
| Unidentified icon, provisionally touchpad toggle | F4 | `KEY_TOUCHPAD_TOGGLE` | vendor consumer 0x0392 |
| Screenshot | F5 | `KEY_SYSRQ` | keyboard 0x46 |
| Lock | F6 | `KEY_SCREENLOCK` | vendor consumer 0x038e |
| Previous track | F7 | `KEY_PREVIOUSSONG` | consumer 0x00b6 |
| Play/pause | F8 | `KEY_PLAYPAUSE` | consumer 0x00cd |
| Next track | F9 | `KEY_NEXTSONG` | consumer 0x00b5 |
| Mute | F10 | `KEY_MUTE` | consumer 0x00e2 |
| Volume up | F11 | `KEY_VOLUMEUP` | consumer 0x00e9 |
| Volume down | F12 | `KEY_VOLUMEDOWN` | consumer 0x00ea |

The fourth key's identity is inferred from the stock touchpad-toggle usage,
not yet confirmed on hardware. If it does not emit F4, capture its `MSC_SCAN`
value in `evtest`. The driver now emits media scancodes even for unmapped
usages; keyboard scancodes are emitted on press. Values use HID-style page
prefixes (`0x0c0000 | usage` for media, `0x070000 | usage` for keyboard).

The output keycode is latched on press and retained until that physical usage
is released. Releasing Fn before the other key, pressing Fn after a held F
key, or moving a held key between report slots does not change its release
code or leave a key stuck. The protocol has two media slots; Fn plus one media
key fits that limit. Host tests exercise these cases, Ctrl combinations,
the 12-position row, ordinary Esc/Delete, repeated/duplicate reports, unknown
usages and truncated reports. They do not establish physical-key behavior.

```sh
python3 external/out_tree_module-sc8547/scripts/test-pogo-keys.py
```

Use `evtest` on **OnePlus Pogo Keyboard** to check Esc, search, all 12 positions,
then search+F1/F2/F5/F11/F12. Test both release orders. Also check typing and
touchpad use after replacing the module. Desktop actions for the Fn layer
depend on desktop bindings and available hardware; emitting mic-mute, for
example, does not add microphone support.

## Build the test image

Keep the already-tested stage4 touch module. Compile only the pogo driver:

```sh
mkdir -p build/nt36532e-stage5/module/pogo
cp external/out_tree_module-sc8547/pogo/oneplus_pogo.c \
    build/nt36532e-stage5/module/pogo/
cat > build/nt36532e-stage5/module/Makefile <<'EOF'
obj-m += oneplus_pogo.o
oneplus_pogo-y := pogo/oneplus_pogo.o
EOF
make -C linux/out M="$PWD/build/nt36532e-stage5/module" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

python3 external/out_tree_module-sc8547/scripts/build-nt36532e-test.py \
    --baseline mainline-boot-v2-wifi-deferred-hmt1-v9-official-bdf.img \
    --module build/nt36532e-stage4/module/nt36532e_ts.ko \
    --pogo-module build/nt36532e-stage5/module/oneplus_pogo.ko \
    --firmware firmware-assets/novatek/DT-novatek-nt36532.bin \
    --output mainline-boot-v2-stage5-pogo-fn-pen-diag-wifi-v9.img
```

The optional `--pogo-module` allows replacement of exactly
`lib/modules/oneplus_pogo.ko`, in addition to the existing init hook/touch
payload. The builder checks the module's name, vermagic and dependencies,
records the old/new module hashes, and verifies all remaining original archive
records byte for byte. Stage5 uses the exact stage4 touch module and preserves
the v9 kernel outside initramfs, WLAN modules/firmware and WLAN init commands.
The helper includes passive CPS power-pin diagnostics; wireless pen charging
is still not implemented. See [CPS8601 investigation](cps8601-bringup.md).
