# Caihong NT36532E bring-up

Status: 2026-09-23. Stage2 is packaged and checked, awaiting device testing.
No successful touchscreen or pen hardware test has been recorded yet.

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

## Stage2 image

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

- Add `nt36532e_ts.ko` and `novatek/DT-novatek-nt36532.bin` to the embedded
  initramfs. Every existing record except `/init` is preserved byte for byte,
  including modes, ownership and links.
- Append the touch startup hook after the original WLAN load and before the
  root transition. Removing this hook recovers the original `/init` exactly.
  The hook copies only touch firmware into the Arch rootfs at
  `/usr/lib/firmware/novatek/`, for subsequent resume firmware requests.
- Add GPIO161 active-low reset and `novatek,pen-support` to the existing touch
  node. The sorted DT comparison permits only those two additions.

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
mkdir -p build/nt36532e-stage2/module/touchscreen
cp external/out_tree_module-sc8547/touchscreen/nt36532e.c \
    build/nt36532e-stage2/module/touchscreen/
cat > build/nt36532e-stage2/module/Makefile <<'EOF'
obj-m += nt36532e_ts.o
nt36532e_ts-y := touchscreen/nt36532e.o
EOF
make -C linux/out M="$PWD/build/nt36532e-stage2/module" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

python3 external/out_tree_module-sc8547/scripts/build-nt36532e-test.py \
    --baseline mainline-boot-v2-wifi-deferred-hmt1-v9-official-bdf.img \
    --module build/nt36532e-stage2/module/nt36532e_ts.ko \
    --firmware firmware-assets/novatek/DT-novatek-nt36532.bin \
    --output mainline-boot-v2-nt36532e-stage2-wifi-v9.img
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

The firmware was extracted from the stock Caihong `firmware-data-0` property
in `caihong-oplus-tp-23926_firmware.dtsi`. It is 249856 bytes with SHA256
`fc6f5124d7f571f1090731b77fff0dcaf0abacbc2249b0dfa4891578919b1788`.

## Device checks

After booting stage2, first confirm Wi-Fi still connects. Then collect:

```sh
sudo dmesg | grep -Ei 'caihong-touch|nt36532|novatek|ath12k'
lsmod | grep -E 'nt36532|ath12k'
cat /proc/bus/input/devices
```

`caihong-touch: nt36532e_ts module inserted` means only driver registration
succeeded. `caihong-touch: device bound: spi...` plus Novatek input devices
shows that probe completed. If there is no bound device, retain the associated
SPI/firmware error logs; do not infer success from `lsmod` alone. The hook
records both console output and kernel messages, including insertion errors.

If input devices exist but touch is still ineffective, inspect their event
streams with `evtest` and retain `/proc/interrupts` before and after touching
the panel. Touch orientation, pen, and suspend/resume need separate tests.
