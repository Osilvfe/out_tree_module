# Caihong boot-slot checkpoint

On 2026-09-24 the user reported that the reboot between CPS Stage9 and Stage9a
unexpectedly switched slots, then restored Linux startup. Read-only checks on
the restored system found:

- The running kernel is the expected `7.2.0-00012-gb35f5cb0b661-dirty`.
- The first 123625472 bytes of `boot_b` match the known working Stage6b image,
  SHA256 `af13c4147d9201cf277e338d6072d646a1acf6609ab2594b4fb9f636e9bb3285`.
- Qualcomm GPT attributes mark `boot_b` active, but not successful:
  `0x0037000000000000`. `boot_a` is inactive and successful:
  `0x007a000000000000`.
- The boot LUN's primary GPT header and partition-array CRCs are valid. Its
  backup header CRC is valid, but its partition-array CRC is invalid. The two
  arrays differ in just one byte: `boot_b` has attribute byte `0x37` in the
  primary and `0x3f` in the backup. The other five LUNs have valid, matching
  primary/backup arrays.

Qualcomm defines slot-active, boot-successful and unbootable as bits 2, 6 and
7 of the attribute byte at bit 48. The missing successful flag is consistent
with exhausting boot retries and falling back to another slot. No capture
exists from immediately before that fallback, so this does not establish the
exact earlier bootloader decision or the origin of the stale backup CRC.
The AOSP `misc` boot-control record is empty on this device; it is not the
place to fix these Qualcomm attributes. The installed `bootctl` executable
is systemd's tool, not Android's boot-control client.

References inspected:

- [Qualcomm boot-control implementation](https://github.com/LineageOS/android_hardware_qcom_bootctrl/tree/lineage-23.2-caf)
  (`boot_control.cpp`, `gpt-utils.h`).
- [Linux qbootctl](https://github.com/linux-msm/qbootctl/tree/39a6e6daaf029fb0a083777679a15ea2c18f72de).

## Prepared repair, not applied

Raw MBRs, GPT headers and entry arrays for all six LUNs are saved on the tablet
and in the local workspace. They are private diagnostic artifacts, not part
of this repository. The backup and proposed repair below were prepared before
the user's decision to leave the boot-success flag alone.

An offline preview sets only the primary `boot_b` successful bit, yielding
`0x0077000000000000`, copies that validated entry array to the backup's
existing location, and recalculates the two entry CRCs and header CRCs. Both
copies then validate. The preview preserves the original MBR, header fields
other than CRCs, partition locations/sizes/types/GUIDs, and other attributes.
It neither changes the selected slot nor replaces any boot image.

A generic `sfdisk --part-attrs` operation was tested only on a sparse local
copy. It also normalized the protective MBR and relocated the backup entry
array by one logical sector. That broader rewrite is not the prepared repair
and has not been run on the tablet. Any application of the narrow repair
must recheck the saved preimage and known boot image, write and verify the
backup before updating the primary, and read back both GPT copies afterward.

The user explicitly declined adding the boot-success flag. The preview stays
unapplied; no tool wrote GPT or slot attributes. A single reboot for Stage9b
returned normally to the expected Linux kernel, with Wi-Fi, pen connection
and scan mode 1 restored. The working Wi-Fi/touch/pen input configuration is
retained, and the paired-pen recovery timer remains enabled. The metadata
findings remain recorded; that successful reboot does not establish that
future boot retries cannot trigger another fallback.
