# Automatic OPN2402 charging

Status: the automatic supervisor is installed and enabled on the running
tablet for the tested Stage9c hardware path. It runs from the writable Arch
rootfs after boot; it is not part of the boot image and does not change the
Wi-Fi, touchscreen or boot-slot metadata.

The timer starts a bounded attempt every 45 seconds. Each attempt:

1. Skips a full or unavailable battery reading, an unready touchscreen, a
   running scan diagnostic, or an active manual CPS module.
2. Loads the tested `caihong_pen_power-stage9c.ko` with `stage=2`.
3. Requests one `charge` transaction. The module validates CPS8601 ID `0x8601`,
   firmware `0x0118`, both pen identity frames, mode 2, protection readbacks
   and telemetry before lowering the charge inhibit GPIO.
4. Keeps the physical operation within the module's existing 15-second cutoff.
   The module cuts inhibit, wake, scan and supply on every exit.
5. Unloads the module and records a root-only state. Failed attempts back off
   for 30, 60, 120, 300 and 600 seconds; a successful attempt clears backoff.

The user-space monitor also terminates the request if it sees removal, a stop
packet, EPT, a fault/undefined interrupt, VIN outside 4000-6500, IIN above
250, or temperature above 45. These limits are stricter than the kernel's
last-resort limits. `systemctl stop caihong-pen-charge.timer` prevents future
attempts; stopping the active service causes the child request to terminate
and the kernel cleanup path to run.

Install on the running rootfs as root:

```sh
install -d -m 0755 /usr/local/lib/caihong
install -m 0700 caihong_pen_power-stage9c.ko /usr/local/lib/caihong/caihong_pen_power.ko
install -m 0755 scripts/caihong-pen-charge.py /usr/local/sbin/caihong-pen-charge
install -m 0644 scripts/caihong-pen-charge.service /etc/systemd/system/
install -m 0644 scripts/caihong-pen-charge.timer /etc/systemd/system/
systemd-analyze verify /etc/systemd/system/caihong-pen-charge.service /etc/systemd/system/caihong-pen-charge.timer
systemctl daemon-reload
systemctl enable --now caihong-pen-charge.timer
```

Inspect only non-sensitive state and journal output:

```sh
systemctl list-timers caihong-pen-charge.timer
cat /run/caihong-pen-charge/state.json
journalctl -u caihong-pen-charge.service -b
```

The public state contains no pen address or raw CPS packet. Battery percentage
is sampled through BlueZ before an attempt, while charge permission and
electrical termination are decided by the CPS driver and its status interface.
The first automatic rounds completed with `charging-observed`, zero failures
and clean module removal. Bluetooth readings moved from 90% to 91%, but that
short change is not a controlled energy measurement; sustained battery gain
remains a follow-up validation.
