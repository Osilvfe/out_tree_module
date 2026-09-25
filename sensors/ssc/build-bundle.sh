#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
firmware_dir=${1:-}
output=${2:-}
hexagonrpc_commit=40e50412812adce9dddc2be9a4510ebce05eef89
hexagonrpc_url=${HEXAGONRPC_URL:-https://github.com/linux-msm/hexagonrpc.git}
erofs_fsck=${EROFS_FSCK:-fsck.erofs}
jobs=${JOBS:-$(nproc)}

if [ -z "$firmware_dir" ] || [ -z "$output" ]; then
	echo "usage: $0 EXTRACTED_FIRMWARE_DIR OUTPUT_DIR" >&2
	exit 2
fi

firmware_dir=$(CDPATH= cd -- "$firmware_dir" && pwd)
case "$output" in
	/*) ;;
	*) output=$(pwd)/$output ;;
esac

for tool in git meson ninja aarch64-linux-gnu-gcc pkg-config jq; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "missing host tool: $tool" >&2
		exit 1
	fi
done
if ! pkg-config --exists json-c; then
	echo "missing host dependency: json-c" >&2
	exit 1
fi
if ! command -v "$erofs_fsck" >/dev/null 2>&1 && [ ! -x "$erofs_fsck" ]; then
	echo "missing EROFS extractor: $erofs_fsck" >&2
	exit 1
fi
for image in vendor.img odm.img; do
	if [ ! -f "$firmware_dir/$image" ]; then
		echo "missing firmware image: $firmware_dir/$image" >&2
		exit 1
	fi
done
if [ -e "$output" ]; then
	echo "refusing existing output directory: $output" >&2
	exit 1
fi

work=$output/work
bundle=$output/bundle
source_dir=$work/hexagonrpc
vendor_sensors=$work/vendor-sensors
vendor_adsp=$work/vendor-adsp
odm_sensors=$work/odm-sensors
vendor_input=$work/vendor-input
odm_input=$work/odm-input
root=$bundle/root

mkdir -p "$work" "$bundle/bin" "$bundle/lib" \
	"$root/sensors/config" "$root/sensors/odm-config" \
	"$root/sensors/registry" "$root/sensors/persist/registry" \
	"$root/dsp/adsp" "$root/socinfo" \
	"$vendor_sensors" "$vendor_adsp" "$odm_sensors" \
	"$vendor_input" "$odm_input"

git clone --filter=blob:none "$hexagonrpc_url" "$source_dir"
git -C "$source_dir" fetch origin refs/pull/27/head
git -C "$source_dir" checkout --detach "$hexagonrpc_commit"
git -C "$source_dir" apply --unidiff-zero "$script_dir/caihong-hexagonrpc.patch"

meson setup "$source_dir/build-native" "$source_dir" --buildtype=release
meson compile -C "$source_dir/build-native" -j "$jobs" sscregistrygen
meson setup "$source_dir/build-aarch64" "$source_dir" \
	--cross-file "$script_dir/aarch64-linux-gnu.ini" --buildtype=release
meson compile -C "$source_dir/build-aarch64" -j "$jobs" hexagonrpcd

"$erofs_fsck" --extract="$vendor_sensors" --path=/etc/sensors \
	--no-preserve "$firmware_dir/vendor.img"
"$erofs_fsck" --extract="$vendor_adsp" --path=/lib/rfsa/adsp \
	--no-preserve "$firmware_dir/vendor.img"
"$erofs_fsck" --extract="$odm_sensors" --path=/etc/sensor \
	--no-preserve "$firmware_dir/odm.img"

copy_list()
{
	list=$1
	source=$2
	stage=$3
	destination=$4

	sed 's/\r$//' "$list" | while IFS= read -r file; do
		if [ -f "$source/$file" ]; then
			cp "$source/$file" "$stage/$file"
			cp "$source/$file" "$destination/$file"
		fi
	done
}

copy_list "$vendor_sensors/config/json.lst" "$vendor_sensors/config" \
	"$vendor_input" "$root/sensors/config"
copy_list "$odm_sensors/config/json_list" "$odm_sensors/config" \
	"$odm_input" "$root/sensors/config"
cp -a "$odm_sensors/config/." "$root/sensors/odm-config/"
cp -a "$vendor_adsp/." "$root/dsp/adsp/"

for list in "$vendor_sensors/config/json.lst" "$odm_sensors/config/json_list"; do
	base=$(dirname -- "$list")
	sed 's/\r$//' "$list" | while IFS= read -r file; do
		if [ -f "$base/$file" ]; then
			printf '%s\n' "$file"
		fi
	done
done > "$root/sensors/config/json.lst"

sed 's#file=config=/odm/etc/sensor/config#file=config=/vendor/etc/sensors/config#' \
	"$vendor_sensors/sns_reg_config" > "$root/sensors/sns_reg.conf"

registrygen=$source_dir/build-native/tools/sscregistrygen
"$registrygen" -p MTP -s 577 "$vendor_input" "$root/sensors/registry"
"$registrygen" -p MTP -s 577 "$odm_input" "$root/sensors/registry"
cp -a "$root/sensors/registry/." "$root/sensors/persist/registry/"

printf 'MTP\n' > "$root/socinfo/hw_platform"
printf '0\n' > "$root/socinfo/platform_subtype"
printf '0\n' > "$root/socinfo/platform_subtype_id"
printf '65536\n' > "$root/socinfo/platform_version"
printf '577\n' > "$root/socinfo/soc_id"
printf '2.0\n' > "$root/socinfo/revision"

install -m 0755 "$source_dir/build-aarch64/hexagonrpcd/hexagonrpcd" \
	"$bundle/bin/hexagonrpcd"
install -m 0755 "$source_dir/build-aarch64/libhexagonrpc/libhexagonrpc.so.0.5" \
	"$bundle/lib/libhexagonrpc.so.0.5"

find "$root/sensors/registry" -maxdepth 1 -type f -exec jq empty {} +
printf 'hexagonrpc %s\n' "$hexagonrpc_commit" > "$bundle/VERSION"
printf 'registry files: '
find "$root/sensors/registry" -maxdepth 1 -type f | wc -l
echo "bundle ready: $bundle"
