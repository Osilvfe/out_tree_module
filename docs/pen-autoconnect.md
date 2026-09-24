# Caihong paired-pen recovery

Status: 2026-09-24. The rootfs service is installed and its timer enabled on
the test tablet. Live tests verify restoration from scan mode 0 to the tested
mode 1, respect for the scan diagnostic's lock, and reconnection after a
deliberate Bluetooth disconnection. A full reboot also passed: the bonded pen
was connected automatically and the service restored scan mode 1 without a
manual command. The user confirmed drawing and pressure work in Krita after
this reboot without manual connection or scan commands.
Basic pressure drawing in native Wayland Krita and pen sleep/resume were
already confirmed by the user.

## Behavior

`caihong-pen-autoconnect.timer` runs a bounded oneshot service 15 seconds after
boot and 15 seconds after each completed run. The helper:

- waits for exactly one bound NT36532E controller to be awake and ready;
- requires an exact configured address already paired in BlueZ, on a powered
  adapter, with the device unblocked;
- restores the explicitly configured scan mode only when it differs;
- uses `Device1.Connect` when the known pen is disconnected;
- backs off failed connection attempts by 30, 60, 120, 240 and then 300 seconds;
- limits each Connect call to 20 seconds and cancels its pending request on
  timeout, preserving a connection that completed at the timeout boundary;
- leaves another client's `InProgress` request alone and shares the existing
  `/run/lock/caihong-pen-scan.lock` with the scan diagnostic.

The helper uses typed BlueZ D-Bus calls via Python GObject/Gio. It does not
discover or pair devices, change trust, power an adapter on, or access CPS8601,
HBOOST, GPIOs or firmware. A manually powered-off adapter is respected.
An absent, attached/asleep or out-of-range pen may need to be removed and
moved before it can accept a connection. Existing Bluetooth bond files stay
under BlueZ's management. The private configured address is not logged.

The kernel driver already retains its scan selection across panel/touch
restarts. This service supplies the selection lost at a full system reboot
and retries link establishment when the pen is available. It does not turn
the manual CPS diagnostic into an automatic attachment or charging driver.
The working boot image, touch module and Wi-Fi payload are unchanged.

## Install on the paired tablet

First complete the validated
[discovery/pairing procedure](cps8601-attachment.md#reproduce-discovery-and-connection).
Only configure a scan mode established by actual input tests; this OPN2402
setup uses **1**, despite its stock Havon label.

From the repository root:

```sh
sudo pacman -S --needed bluez python python-gobject
sudo install -m 0755 scripts/caihong-pen-autoconnect.py /usr/local/sbin/caihong-pen-autoconnect
sudo install -m 0644 scripts/caihong-pen-autoconnect.service /etc/systemd/system/
sudo install -m 0644 scripts/caihong-pen-autoconnect.timer /etc/systemd/system/
```

Create `/etc/caihong-pen.json`, owned by root with mode `0600`. Replace the
placeholder with the already paired pen's exact address:

```json
{"address": "<paired-pen-address>", "scan_mode": 1}
```

```sh
sudo chown root:root /etc/caihong-pen.json
sudo chmod 0600 /etc/caihong-pen.json
sudo systemd-analyze verify /etc/systemd/system/caihong-pen-autoconnect.service /etc/systemd/system/caihong-pen-autoconnect.timer
sudo systemctl daemon-reload
sudo systemctl enable --now bluetooth.service
sudo systemctl enable --now caihong-pen-autoconnect.timer
```

The service is installed on the real rootfs. The existing initramfs does not
overwrite these files; rebuilding or flashing the boot image is unnecessary.
The script requires Python GObject/Gio; `python-dbus` is not required.

## Check and stop

```sh
systemctl list-timers caihong-pen-autoconnect.timer
sudo cat /run/caihong-pen-autoconnect/state.json
journalctl -u caihong-pen-autoconnect.service -b
cat /sys/bus/spi/devices/spi0.0/pen_scan
```

An inactive oneshot service between timer runs is normal. `status=connected`
in the JSON means the Bluetooth link was up and the scan mode matched on
that run; it does not prove fresh pen coordinates. Verify actual drawing or
increasing `pen_stats` reports separately. Retry state is private, stored in
`/run`, and cleared by reboot. The timer does not wake a suspended system.

Before manual scan-mode experiments, or to return to manual operation:

```sh
sudo systemctl disable --now caihong-pen-autoconnect.timer
sudo systemctl stop caihong-pen-autoconnect.service
```

The helper's lock prevents concurrent diagnostic writes, but an enabled timer
will reapply its configured mode after a diagnostic finishes. Stopping the
service preserves the current scan mode and existing Bluetooth bond/link.

## Validation

`python3 scripts/test-caihong-pen-autoconnect.py` covers readiness, explicit
pairing/address selection, radio-off/blocked states, idempotence, backoff,
late connection success, pending-request ownership, timeout cancellation,
configuration parsing and scan failure. Fourteen cases passed.

The installed systemd unit passed `systemd-analyze verify`. On the live tablet,
a service run while the diagnostic lock was held left mode 0 alone; the next
run restored mode 1. Disconnecting the known paired pen and invoking the
service restored the Bluetooth connection. After a full reboot, its first run
restored mode 1 and found the pen already connected; the boot log therefore
proves automatic link recovery, not that this service itself issued the boot's
Connect call. The installed configuration and enabled timer persisted, and
Wi-Fi reconnected. The user then confirmed normal drawing and pressure in
Krita on this new boot. Long absences and long-term connection stability
remain separate tests.
