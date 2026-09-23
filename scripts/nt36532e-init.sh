# Sourced by the v9 /init after its existing deferred WLAN load, before
# switch_root. Keep all WLAN commands and their relative order intact.
touch_log()
{
	log "caihong-touch: $*"
	printf '<6>caihong-touch: %s\n' "$*" > /dev/kmsg
}

touch_log "Wi-Fi v9 baseline; preparing NT36532E touch and pen with panel sequencing"

if mkdir -p /newroot/usr/local/sbin &&
	cp /usr/local/sbin/caihong-pen-status /newroot/usr/local/sbin/caihong-pen-status &&
	chmod 0755 /newroot/usr/local/sbin/caihong-pen-status; then
	touch_log "installed caihong-pen-status diagnostic helper"
else
	touch_log "could not install pen diagnostic helper"
fi

# Arch uses /lib -> usr/lib. Write through the real rootfs path rather than
# following a possibly absolute /newroot/lib symlink from the initramfs.
# Stage4 retains firmware in RAM; keep the file available for module reprobes.
touch_fw=DT-novatek-nt36532.bin
touch_fw_src=/lib/firmware/novatek/$touch_fw
touch_fw_dir=/newroot/usr/lib/firmware/novatek
if mkdir -p "$touch_fw_dir" &&
	cp "$touch_fw_src" "$touch_fw_dir/$touch_fw.new" &&
	chmod 0644 "$touch_fw_dir/$touch_fw.new" &&
	mv "$touch_fw_dir/$touch_fw.new" "$touch_fw_dir/$touch_fw"; then
	touch_log "installed rootfs touch firmware for module reprobes"
else
	touch_log "ERROR: rootfs touch firmware installation failed; later reprobes may fail"
fi

if insmod /lib/modules/nt36532e_ts.ko; then
	touch_log "nt36532e_ts module inserted"
else
	touch_rc=$?
	touch_log "ERROR: nt36532e_ts insmod failed: $touch_rc"
fi

# A successful insmod registers the driver even when device probe fails.
touch_bound=0
for touch_device in /sys/bus/spi/drivers/nt36532e/spi*; do
	[ -L "$touch_device" ] || continue
	touch_log "device bound: ${touch_device##*/}"
	touch_bound=1
done
if [ "$touch_bound" -eq 0 ]; then
	touch_log "no SPI device bound yet; check nt36532e probe/deferred-probe logs"
fi

# Registration holds CPS8601 off. Only the explicit probe_once sysfs command
# performs a bounded power/ID test; there is no charging or firmware update.
if [ -f /lib/modules/caihong_pen_power.ko ]; then
	if insmod /lib/modules/caihong_pen_power.ko; then
		touch_log "pen power diagnostic inserted; manual probe only"
	else
		touch_log "ERROR: pen power diagnostic insertion failed"
	fi
fi
