#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Add the NT36532E bring-up payload to the pinned, working Caihong Wi-Fi v9.

Only the embedded initramfs and listed touchscreen DT properties may change.
This deliberately does not rebuild the kernel or consume a mutable ramdisk
directory. Requires Python 3, dtc, fdtget, fdtput, modinfo and mkbootimg.
"""

import argparse
from dataclasses import dataclass
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib


BASELINE_SHA256 = "0dcedad3958e689331881d791bbfecafd7055905d628f9b1a776dbb8ad6b15ae"
FIRMWARE_SHA256 = "fc6f5124d7f571f1090731b77fff0dcaf0abacbc2249b0dfa4891578919b1788"
TOUCH_NODE = "/soc@0/geniqup@ac0000/spi@a90000/touchscreen@0"
MODULE = "lib/modules/nt36532e_ts.ko"
FIRMWARE = "lib/firmware/novatek/DT-novatek-nt36532.bin"
PEN_STATUS = "usr/local/sbin/caihong-pen-status"
ANCHOR = b"mkdir -p /newroot/dev /newroot/proc /newroot/sys /newroot/run\n"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def align(value, size=4):
    return (value + size - 1) & ~(size - 1)


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


@dataclass
class Entry:
    name: str
    fields: list
    data: bytes
    raw: bytes


def parse_cpio(blob, offset=0):
    """Parse newc without extracting any path; retain original record bytes."""
    entries = {}
    start = offset
    while True:
        require(blob[offset:offset + 6] == b"070701", "invalid newc magic")
        header = blob[offset + 6:offset + 110]
        require(re.fullmatch(rb"[0-9a-fA-F]{104}", header), "invalid newc header")
        fields = [int(header[i:i + 8], 16) for i in range(0, 104, 8)]
        size, namesize = fields[6], fields[11]
        require(namesize > 0, "empty newc name")
        name_end = offset + 110 + namesize
        require(blob[name_end - 1:name_end] == b"\0", "unterminated newc name")
        name = blob[offset + 110:name_end - 1].decode()
        data_start = align(name_end)
        end = align(data_start + size)
        require(end <= len(blob), "truncated newc record")
        require(name not in entries, f"duplicate newc entry: {name}")
        entries[name] = Entry(name, fields, blob[data_start:data_start + size],
                              blob[offset:end])
        offset = end
        if name == "TRAILER!!!":
            require(size == 0, "nonempty newc trailer")
            return entries, offset - start


def encode_entry(name, fields, data):
    fields = list(fields)
    name_bytes = name.encode() + b"\0"
    fields[6], fields[11] = len(data), len(name_bytes)
    record = b"070701" + b"".join(f"{v:08x}".encode() for v in fields) + name_bytes
    record += b"\0" * (align(len(record)) - len(record))
    record += data
    return record + b"\0" * (align(len(record)) - len(record))


def split_boot(blob):
    require(blob[:8] == b"ANDROID!", "not an Android boot image")
    require(struct.unpack_from("<I", blob, 40)[0] == 2, "requires boot header v2")
    kernel_size, = struct.unpack_from("<I", blob, 8)
    page, = struct.unpack_from("<I", blob, 36)
    require(page == 4096, "unexpected boot page size")
    for offset in (16, 24, 1632):
        require(struct.unpack_from("<I", blob, offset)[0] == 0,
                "external ramdisk/second/recovery payload not supported")
    dtb_size, = struct.unpack_from("<I", blob, 1648)
    dtb_start = align(page + kernel_size, page)
    require(len(blob) == align(dtb_start + dtb_size, page),
            "unexpected boot size or trailing signature")
    return blob[:page], blob[page:page + kernel_size], blob[dtb_start:dtb_start + dtb_size]


def boot_id(kernel, dtb):
    digest = hashlib.sha1()
    for payload in (kernel, b"", b"", b"", dtb):
        digest.update(payload)
        digest.update(struct.pack("<I", len(payload)))
    return digest.digest() + b"\0" * 12


def patch_dtb(original, work):
    source, dest = work / "v9.dtb", work / "touch.dtb"
    source.write_bytes(original)
    dest.write_bytes(original)
    require(run("fdtget", source, TOUCH_NODE, "compatible") == "novatek,nt36532e",
            "unexpected touch compatible")
    irq = run("fdtget", "-t", "x", source, TOUCH_NODE, "interrupts-extended").split()
    require(irq[1:] == ["a2", "2"], "unexpected touch IRQ GPIO/trigger")
    run("fdtput", "-t", "x", dest, TOUCH_NODE, "reset-gpios", irq[0], "a1", "1")
    run("fdtput", dest, TOUCH_NODE, "novatek,pen-support")
    pen_properties = {"novatek,pen-max-pressure": 16383, "novatek,pen-max-tilt": 60,
                      "touchscreen-x-mm": 177, "touchscreen-y-mm": 250}
    for name, value in pen_properties.items():
        run("fdtput", "-t", "x", dest, TOUCH_NODE, name, f"{value:x}")
    before = run("dtc", "-q", "-s", "-I", "dtb", "-O", "dts", source)
    after = run("dtc", "-q", "-s", "-I", "dtb", "-O", "dts", dest)
    added = [f"\t\t\t\t\treset-gpios = <0x{irq[0]} 0xa1 0x01>;\n",
             "\t\t\t\t\tnovatek,pen-support;\n"]
    added += [f"\t\t\t\t\t{name} = <0x{value:x}>;\n" for name, value in pen_properties.items()]
    restored = after
    for line in added:
        require(restored.count(line) == 1, f"unexpected DT property: {line.strip()}")
        restored = restored.replace(line, "", 1)
    require(restored == before, "DT changes extend beyond the listed touch/pen properties")
    return dest


def verify_image(blob, baseline, archive_offset, archive_size, expected, dtb):
    old_header, old_kernel, _ = split_boot(baseline)
    header, kernel, actual_dtb = split_boot(blob)
    require(len(kernel) == len(old_kernel), "kernel size changed")
    require(kernel[:archive_offset] == old_kernel[:archive_offset] and
            kernel[archive_offset + archive_size:] == old_kernel[archive_offset + archive_size:],
            "kernel bytes outside initramfs changed")
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    archive = decoder.decompress(kernel[archive_offset:archive_offset + archive_size])
    require(decoder.eof and not any(decoder.unused_data), "invalid gzip or nonzero padding")
    actual, size = parse_cpio(archive)
    require(size == len(archive), "trailing bytes in decompressed cpio")
    require(actual.keys() == expected.keys(), "archive file list changed unexpectedly")
    for name in expected:
        require(actual[name].raw == expected[name].raw, f"archive mismatch: {name}")
    require(actual_dtb == dtb, "DTB mismatch")
    restored = bytearray(header)
    restored[576:608] = old_header[576:608]
    restored[1648:1652] = old_header[1648:1652]
    require(restored == old_header, "boot header/cmdline changed unexpectedly")
    require(header[576:608] == boot_id(kernel, actual_dtb), "invalid boot image ID")
    require(len(blob) <= 192 * 1024 * 1024, "image exceeds boot partition")


def build(args):
    require(not args.output.exists(), f"refusing to overwrite {args.output}")
    baseline = args.baseline.read_bytes()
    require(sha256(baseline) == BASELINE_SHA256, "baseline SHA256 does not match working Wi-Fi v9")
    firmware = args.firmware.read_bytes()
    require(sha256(firmware) == FIRMWARE_SHA256, "unexpected Caihong touch firmware SHA256")
    header, kernel, dtb = split_boot(baseline)
    require(header[576:608] == boot_id(kernel, dtb), "invalid baseline boot image ID")
    config_start = kernel.index(b"IKCFG_ST") + 8
    config_end = kernel.index(b"IKCFG_ED", config_start)
    config = gzip.decompress(kernel[config_start:config_end])
    require(b"\nCONFIG_RD_GZIP=y\n" in config, "baseline cannot unpack gzip initramfs")
    # The kernel also contains the string literal "070701". Require a full
    # newc header rather than replacing the first occurrence of that string.
    match = re.search(rb"070701[0-9a-fA-F]{104}", kernel)
    require(match is not None, "embedded newc archive not found")
    archive_offset = match.start()
    require(archive_offset % 4 == 0, "unaligned baseline initramfs")
    original, archive_size = parse_cpio(kernel, archive_offset)
    hook = Path(__file__).with_name("nt36532e-init.sh").read_bytes()
    init = original["init"].data
    require(init.count(ANCHOR) == 1, "missing or ambiguous init insertion point")
    require(b"loaded deferred ath12k" in init[:init.index(ANCHOR)], "missing v9 WLAN load")
    new_init = init.replace(ANCHOR, hook + b"\n" + ANCHOR, 1)
    require(new_init.replace(hook + b"\n", b"", 1) == init, "original init was modified")
    module = args.module.read_bytes()
    with tempfile.TemporaryDirectory(prefix="nt36532e-", dir=args.output.parent) as temp:
        work = Path(temp)
        pogo = work / "oneplus_pogo.ko"
        pogo.write_bytes(original["lib/modules/oneplus_pogo.ko"].data)
        vermagic = run("modinfo", "-F", "vermagic", args.module)
        require(vermagic == run("modinfo", "-F", "vermagic", pogo), "module vermagic mismatch")
        require(run("modinfo", "-F", "name", args.module) == "nt36532e_ts", "incorrect module name")
        require(not run("modinfo", "-F", "depends", args.module), "unhandled module dependencies")
        records = [encode_entry(name, entry.fields, new_init) if name == "init" else entry.raw
                   for name, entry in original.items() if name != "TRAILER!!!"]
        additions = {"lib/firmware/novatek": (0o40755, b""),
                     FIRMWARE: (0o100644, firmware), MODULE: (0o100644, module),
                     PEN_STATUS: (0o100755, Path(__file__).with_name("caihong-pen-status.py").read_bytes())}
        inode = max(entry.fields[0] for entry in original.values()) + 1
        for name, (mode, data) in additions.items():
            require(name not in original, f"touch payload already exists: {name}")
            fields = [inode, mode, 0, 0, 2 if mode == 0o40755 else 1, 0, 0, 0, 0, 0, 0, 0, 0]
            records.append(encode_entry(name, fields, data))
            inode += 1
        records.append(original["TRAILER!!!"].raw)
        new_archive = b"".join(records)
        expected, size = parse_cpio(new_archive)
        require(size == len(new_archive), "incorrect new archive size")
        # Compare the entire records, not just contents: modes, ownership,
        # links and device nodes are as important as module/firmware hashes.
        for name, entry in original.items():
            if name != "init":
                require(expected[name].raw == entry.raw, f"baseline entry changed: {name}")
        init_path = work / "init"
        init_path.write_bytes(new_init)
        run("sh", "-n", init_path)
        compressed = gzip.compress(new_archive, mtime=0)
        require(len(compressed) <= archive_size, "compressed initramfs exceeds reserved space")
        patched_kernel = (kernel[:archive_offset] + compressed +
                          b"\0" * (archive_size - len(compressed)) +
                          kernel[archive_offset + archive_size:])
        image = work / "Image"
        image.write_bytes(patched_kernel)
        patched_dtb = patch_dtb(dtb, work)
        output = work / "boot.img"
        cmdline = (header[64:576].split(b"\0")[0] + header[608:1632].split(b"\0")[0]).decode()
        # Metadata is fixed by the pinned v9 hash; verify the generated header
        # against the baseline rather than trusting local project defaults.
        run("mkbootimg", "--base", "0", "--kernel_offset", "0x8000",
            "--tags_offset", "0x100", "--dtb_offset", "0x4000000",
            "--pagesize", "4096", "--header_version", "2", "--os_version", "16.0.0",
            "--os_patch_level", "2026-06", "--kernel", image, "--dtb", patched_dtb,
            "--cmdline", cmdline, "--output", output)
        result = output.read_bytes()
        verify_image(result, baseline, archive_offset, archive_size, expected, patched_dtb.read_bytes())
        manifest = {
            "image": args.output.name, "sha256": sha256(result), "bytes": len(result),
            "baseline_sha256": BASELINE_SHA256, "module_vermagic": vermagic,
            "initramfs_offset_in_kernel": archive_offset, "initramfs_reserved_bytes": archive_size,
            "initramfs_gzip_bytes": len(compressed), "original_entries": len(original),
            "preserved_entries": len(original) - 1, "replaced_entries": ["init"],
            "added_entries": sorted(additions),
            "checks": ["kernel identical outside embedded initramfs", "original init retained verbatim around hook",
                       "all other original cpio records byte-identical", "module vermagic matches baseline",
                       "DT only adds listed touchscreen reset/pen properties", "boot metadata and cmdline unchanged",
                       "final image decompressed and all records verified", "boot ID verified"],
            "hardware_test": "pending: boot, Wi-Fi, touch, pen and suspend/resume",
            "wifi_sha256": {name: sha256(entry.data) for name, entry in original.items()
                            if "/ath12k/" in name and entry.data},
            "touch_sha256": {MODULE: sha256(module), FIRMWARE: sha256(firmware), "init": sha256(new_init)},
        }
        output.rename(args.output)
        args.output.with_suffix(".img.json").write_text(json.dumps(manifest, indent=2) + "\n")
        args.output.with_suffix(".img.sha256").write_text(f"{manifest['sha256']}  {args.output.name}\n")
        print(json.dumps(manifest, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        build(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
