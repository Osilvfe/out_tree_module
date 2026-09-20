# Kernel-side prerequisites

These patches preserve the kernel-side portions that cannot safely be
implemented by an ordinary out-of-tree child driver.

- `0001-power-supply-qcom-battmgr-oneplus-pps-wip.patch` is the frozen
  experimental Stage 6/7 implementation. It is archived for reproducibility,
  not recommended for normal use, and should be reduced to a minimal serialized
  transport API before charging development resumes.
- `0002-serial-qcom-geni-add-force-fifo-mode.patch` adds a generic DT-selected
  FIFO transfer mode required before the pogo serdev child binds.

Both patches are based on the local Linux v7.2 Caihong tree. Apply from the
kernel repository root with `git apply --check` before `git apply`.
