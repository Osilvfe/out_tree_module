# Kernel-side snapshots

These patches preserve historical snapshots of kernel-side portions that
cannot safely be implemented by an ordinary child module. “Out-of-tree” in
this repository means not merged upstream; the active Caihong integration
still lives in the companion kernel tree.

- `0001-power-supply-qcom-battmgr-oneplus-pps-wip.patch` is the frozen
  pre-refactor Stage 6/7 implementation. It is archived for reproducibility,
  not recommended for normal use, and is not the current source of truth. The
  companion tree now isolates policy in Caihong-specific files while retaining
  only serialized owner hooks in `qcom_battmgr.c`.
- `0002-serial-qcom-geni-add-force-fifo-mode.patch` adds a generic DT-selected
  FIFO transfer mode required before the pogo serdev child binds.

Both snapshots are based on the local Linux v7.2 Caihong tree. They are useful
for history and comparison; do not apply them on top of the current companion
tree, which already contains the integration in its refactored form.
