#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
bundle=${1:-}
target=${2:-}

if [ -z "$bundle" ] || [ -z "$target" ]; then
	echo "usage: $0 BUNDLE_DIR ROOTFS_DIR" >&2
	exit 2
fi
bundle=$(CDPATH= cd -- "$bundle" && pwd)
target=$(CDPATH= cd -- "$target" && pwd)

for file in bin/hexagonrpcd lib/libhexagonrpc.so.0.5 root/sensors/sns_reg.conf; do
	if [ ! -f "$bundle/$file" ]; then
		echo "incomplete SSC bundle: missing $file" >&2
		exit 1
	fi
done

install -D -m 0755 "$bundle/bin/hexagonrpcd" \
	"$target/usr/local/libexec/caihong-ssc/hexagonrpcd"
install -D -m 0755 "$bundle/lib/libhexagonrpc.so.0.5" \
	"$target/usr/local/lib/caihong-ssc/libhexagonrpc.so.0.5"
install -D -m 0644 "$script_dir/caihong-ssc.service" \
	"$target/usr/lib/systemd/system/caihong-ssc.service"
install -D -m 0644 "$script_dir/90-caihong-ssc.rules" \
	"$target/usr/lib/udev/rules.d/90-caihong-ssc.rules"
install -D -m 0644 "$script_dir/iio-sensor-proxy.conf" \
	"$target/usr/lib/systemd/system/iio-sensor-proxy.service.d/caihong-ssc.conf"

state=$target/var/lib/caihong-ssc/root
if [ -e "$state" ] && [ "${FORCE:-0}" != 1 ]; then
	echo "refusing existing SSC state: $state (set FORCE=1 to overlay)" >&2
	exit 1
fi
mkdir -p "$state"
cp -a "$bundle/root/." "$state/"

mkdir -p "$target/etc/systemd/system/multi-user.target.wants"
ln -sfn /usr/lib/systemd/system/caihong-ssc.service \
	"$target/etc/systemd/system/multi-user.target.wants/caihong-ssc.service"

echo "installed Caihong SSC support into $target"
