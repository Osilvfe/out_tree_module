#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only evidence collector for caihong SC8547 staged hardware validation.
# This script intentionally performs no sysfs/device-tree/kernel control write.

set -u

usage()
{
	cat <<'EOF'
Usage:
  sc8547-stage-capture.sh <stage> [label] [output-dir]

Examples:
  sc8547-stage-capture.sh 1 unplugged
  sc8547-stage-capture.sh 4 primary-before-enable
  sc8547-stage-capture.sh 5A 5v
  sc8547-stage-capture.sh 6A pps-adapter

Optional environment strings copied into test-context.txt:
  SC8547_KERNEL_COMMITS
  SC8547_DT_COMMITS
  SC8547_ADAPTER
  SC8547_CABLE
  SC8547_NOTES

The collector is read-only. Write-capable test commands remain manual and are
specified in docs/sc8547-stage-test-plan.md.
EOF
}

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
	usage >&2
	exit 2
fi

stage=$1
label=${2:-snapshot}
stamp=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%d-%H%M%S)
out=${3:-"sc8547-stage-${stage}-${label}-${stamp}"}

case "$stage" in
	0|1|2|3|4|5A|5B|6A|6B) ;;
	*)
		echo "unsupported stage label: $stage" >&2
		usage >&2
		exit 2
		;;
esac

mkdir -p "$out"

log()
{
	printf '%s\n' "$*" >&2
}

write_header()
{
	file=$1
	title=$2
	{
		printf '# %s\n' "$title"
		printf '# stage=%s label=%s captured=%s\n\n' "$stage" "$label" "$stamp"
	} > "$file"
}

capture_command()
{
	file=$1
	shift
	write_header "$file" "$*"
	{
		printf '$'
		printf ' %s' "$@"
		printf '\n'
		"$@"
	} >> "$file" 2>&1 || true
}

capture_file()
{
	path=$1
	file=$2
	write_header "$file" "$path"
	if [ -r "$path" ]; then
		cat "$path" >> "$file" 2>&1 || true
	else
		printf 'UNAVAILABLE_OR_UNREADABLE\n' >> "$file"
	fi
}

append_attr()
{
	base=$1
	attr=$2
	file=$3
	path="$base/$attr"

	printf '%s=' "$attr" >> "$file"
	if [ -r "$path" ]; then
		value=$(cat "$path" 2>&1) || value="READ_ERROR:$?"
		printf '%s\n' "$value" >> "$file"
	else
		printf 'UNAVAILABLE\n' >> "$file"
	fi
}

capture_group()
{
	base=$1
	name=$2
	shift 2
	file="$out/$name.txt"

	write_header "$file" "$base"
	printf 'path=%s\n' "$base" >> "$file"
	for attr in "$@"; do
		append_attr "$base" "$attr" "$file"
	done
}

log "SC8547 stage $stage read-only capture -> $out"

# ---------------------------------------------------------------------------
# Test context
# ---------------------------------------------------------------------------
context="$out/test-context.txt"
write_header "$context" "test context"
{
	printf 'stage=%s\n' "$stage"
	printf 'label=%s\n' "$label"
	printf 'timestamp=%s\n' "$stamp"
	printf 'kernel_commits=%s\n' "${SC8547_KERNEL_COMMITS:-UNSPECIFIED}"
	printf 'dt_commits=%s\n' "${SC8547_DT_COMMITS:-UNSPECIFIED}"
	printf 'adapter=%s\n' "${SC8547_ADAPTER:-UNSPECIFIED}"
	printf 'cable=%s\n' "${SC8547_CABLE:-UNSPECIFIED}"
	printf 'notes=%s\n' "${SC8547_NOTES:-}"
	printf '\n'
	uname -a 2>&1 || true
	printf '\n/proc/cmdline:\n'
	cat /proc/cmdline 2>&1 || true
	printf '\n/proc/version:\n'
	cat /proc/version 2>&1 || true
} >> "$context"

capture_command "$out/lsmod.txt" lsmod
capture_file /proc/config.gz "$out/proc-config.gz.txt"

# Preserve the real compressed config when available.
if [ -r /proc/config.gz ]; then
	cp /proc/config.gz "$out/config.gz" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# Module provenance
# ---------------------------------------------------------------------------
mods="$out/modules.txt"
write_header "$mods" "module provenance"
for mod in sc8547_cp sc8547_dual sc8547_policy_diag; do
	printf '\n[%s]\n' "$mod" >> "$mods"
	if command -v modinfo >/dev/null 2>&1; then
		modinfo "$mod" >> "$mods" 2>&1 || true
		path=$(modinfo -n "$mod" 2>/dev/null || true)
		if [ -n "$path" ] && [ -r "$path" ]; then
			if command -v sha256sum >/dev/null 2>&1; then
				sha256sum "$path" >> "$mods" 2>&1 || true
			fi
		fi
	else
		printf 'modinfo unavailable\n' >> "$mods"
	fi
done

# ---------------------------------------------------------------------------
# Physical SC8547 devices
# ---------------------------------------------------------------------------
physical_attrs='device_id variant role charge_enabled charge_mode switching adapter_present battery_present vbus_uv ibus_ua vbat_uv vout_uv vac_uv tdie_mc status_regs faults register_dump protection_state'
experimental_attrs='profile_raw init_state apply_init watchdog_ms enable_window work_mode cp_enable'

physical_index=0
for dev in /sys/bus/i2c/devices/*-006f; do
	[ -d "$dev/sc8547" ] || continue
	physical_index=$((physical_index + 1))
	name=$(basename "$dev")
	file="$out/physical-${physical_index}-${name}.txt"
	write_header "$file" "$dev"
	printf 'device=%s\n' "$dev" >> "$file"

	for attr in $physical_attrs; do
		append_attr "$dev/sc8547" "$attr" "$file"
	done

	if [ -d "$dev/sc8547_experimental" ]; then
		printf '\n[sc8547_experimental]\n' >> "$file"
		for attr in $experimental_attrs; do
			# Reading a write-only attribute is expected to fail/appear unavailable.
			append_attr "$dev/sc8547_experimental" "$attr" "$file"
		done
	else
		printf '\nsc8547_experimental=ABSENT\n' >> "$file"
	fi
done

if [ "$physical_index" -eq 0 ]; then
	printf 'No *-006f device with sc8547 sysfs group found.\n' > "$out/physical-NOT-FOUND.txt"
fi

# ---------------------------------------------------------------------------
# Virtual dual-pump coordinator
# ---------------------------------------------------------------------------
dual_index=0
for group in /sys/bus/platform/devices/*/sc8547_dual; do
	[ -d "$group" ] || continue
	dual_index=$((dual_index + 1))
	name=$(basename "$(dirname "$group")")
	capture_group "$group" "dual-${dual_index}-${name}" \
		peer pair_state aggregate_ibus_ua last_result work_mode dual_enable

done

if [ "$dual_index" -eq 0 ]; then
	printf 'No sc8547_dual platform sysfs group found.\n' > "$out/dual-NOT-FOUND.txt"
fi

# ---------------------------------------------------------------------------
# Stage-6A policy diagnostic
# ---------------------------------------------------------------------------
policy_index=0
for group in /sys/bus/platform/devices/*/sc8547_policy; do
	[ -d "$group" ] || continue
	policy_index=$((policy_index + 1))
	name=$(basename "$(dirname "$group")")
	capture_group "$group" "policy-${policy_index}-${name}" \
		usb_supply source_state combined_state

done

if [ "$policy_index" -eq 0 ]; then
	printf 'No sc8547_policy platform sysfs group found.\n' > "$out/policy-NOT-FOUND.txt"
fi

# ---------------------------------------------------------------------------
# Qualcomm battmgr USB source view
# ---------------------------------------------------------------------------
usb=/sys/class/power_supply/qcom-battmgr-usb
if [ -d "$usb" ]; then
	capture_group "$usb" qcom-battmgr-usb \
		online usb_type voltage_now voltage_max current_now current_max \
		input_current_limit type
else
	printf '%s not found.\n' "$usb" > "$out/qcom-battmgr-usb-NOT-FOUND.txt"
fi

# Capture all power-supply names/types for disambiguation without writing them.
psys="$out/power-supplies.txt"
write_header "$psys" "/sys/class/power_supply inventory"
for psy in /sys/class/power_supply/*; do
	[ -e "$psy" ] || continue
	printf '\n[%s]\n' "$(basename "$psy")" >> "$psys"
	for attr in type online usb_type voltage_now voltage_max current_now current_max input_current_limit; do
		append_attr "$psy" "$attr" "$psys"
	done
done

# ---------------------------------------------------------------------------
# Standard USB Power Delivery capability tree (UCSI/typec evidence)
# ---------------------------------------------------------------------------
pdout="$out/usb-power-delivery.txt"
write_header "$pdout" "/sys/class/usb_power_delivery readable capability tree"
if [ -d /sys/class/usb_power_delivery ]; then
	find -L /sys/class/usb_power_delivery -maxdepth 8 -print 2>&1 >> "$pdout" || true
	printf '\n[readable files]\n' >> "$pdout"
	find -L /sys/class/usb_power_delivery -maxdepth 8 -type f -print 2>/dev/null |
	while IFS= read -r f; do
		[ -r "$f" ] || continue
		printf '\n--- %s ---\n' "$f" >> "$pdout"
		cat "$f" >> "$pdout" 2>&1 || true
	done
else
	printf 'ABSENT\n' >> "$pdout"
fi

# Type-C tree is useful for UCSI/partner correlation.
typecout="$out/typec.txt"
write_header "$typecout" "/sys/class/typec readable inventory"
if [ -d /sys/class/typec ]; then
	find -L /sys/class/typec -maxdepth 6 -print 2>&1 >> "$typecout" || true
	printf '\n[common readable fields]\n' >> "$typecout"
	for node in /sys/class/typec/*; do
		[ -e "$node" ] || continue
		printf '\n[%s]\n' "$(basename "$node")" >> "$typecout"
		for attr in data_role power_role port_type preferred_role usb_power_delivery_revision usb_typec_revision; do
			append_attr "$node" "$attr" "$typecout"
		done
	done
else
	printf 'ABSENT\n' >> "$typecout"
fi

# ---------------------------------------------------------------------------
# Runtime DT evidence for SoCCP/Oplus battmgr extension selection
# ---------------------------------------------------------------------------
dtout="$out/runtime-dt-battmgr.txt"
write_header "$dtout" "runtime DT battmgr/SoCCP evidence"
if [ -d /proc/device-tree ]; then
	find /proc/device-tree \( -iname '*soccp*' -o -iname '*battery*charger*' -o -iname '*pmic*glink*' \) -print 2>&1 >> "$dtout" || true
	printf '\n[soccp-like property contents]\n' >> "$dtout"
	find /proc/device-tree -type f -iname '*soccp*' -print 2>/dev/null |
	while IFS= read -r f; do
		printf '\n--- %s ---\n' "$f" >> "$dtout"
		if command -v od >/dev/null 2>&1; then
			od -An -tx1 -v "$f" >> "$dtout" 2>&1 || true
		else
			cat "$f" >> "$dtout" 2>&1 || true
		fi
	done
else
	printf '/proc/device-tree ABSENT\n' >> "$dtout"
fi

# ---------------------------------------------------------------------------
# Kernel log last, so all reads above are represented in the log when possible.
# ---------------------------------------------------------------------------
capture_command "$out/dmesg.txt" dmesg

# Summary designed to be readable before opening the larger files.
summary="$out/SUMMARY.txt"
write_header "$summary" "capture summary"
{
	printf 'output_dir=%s\n' "$out"
	printf 'physical_devices=%d\n' "$physical_index"
	printf 'dual_groups=%d\n' "$dual_index"
	printf 'policy_groups=%d\n' "$policy_index"
	printf 'qcom_battmgr_usb=%s\n' "$([ -d "$usb" ] && echo present || echo absent)"
	printf 'usb_power_delivery=%s\n' "$([ -d /sys/class/usb_power_delivery ] && echo present || echo absent)"
	printf '\nREAD-ONLY CAPTURE COMPLETE.\n'
	printf 'No sysfs/control write was issued by this script.\n'
} >> "$summary"

log "Capture complete: $out"
