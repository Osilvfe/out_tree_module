#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run one bounded CPS8601 pen-charge observation from a systemd timer.

The kernel module owns the electrical watchdog and cuts the physical path on
every exit. This process gates attempts, monitors the read-only status, and
unloads the module after the bounded request.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


PROVIDER = Path("/sys/bus/platform/devices/caihong-pen-power")
CONTROLLER = Path("/sys/bus/spi/devices/spi0.0")
LOCK = Path("/run/lock/caihong-pen-scan.lock")
DEFAULT_CONFIG = Path("/etc/caihong-pen.json")
DEFAULT_STATE = Path("/run/caihong-pen-charge/state.json")
DEFAULT_MODULE = Path("/usr/local/lib/caihong/caihong_pen_power.ko")
ATTEMPT_TIMEOUT = 22
RETRY_DELAYS = (30, 60, 120, 300, 600)
FULL_BATTERY = 99
FAULT_FLAGS = ((1 << 6) | (1 << 7) | (1 << 8) | (1 << 10) |
               (1 << 11) | (1 << 13) | (1 << 14) | (1 << 15))


class ChargeError(RuntimeError):
    pass


def fields(text):
    result = {}
    for item in text.split():
        if "=" in item:
            key, value = item.split("=", 1)
            result[key] = value
    return result


def integer(data, key, default=None, base=0):
    try:
        return int(data[key], base)
    except (KeyError, TypeError, ValueError):
        return default


def load_config(path):
    data = json.loads(path.read_text())
    if (not isinstance(data, dict) or set(data) != {"address", "scan_mode"}
            or not isinstance(data["address"], str)
            or not isinstance(data["scan_mode"], int)
            or data["scan_mode"] not in range(1, 6)):
        raise ValueError("invalid paired pen configuration")
    return data["address"].upper(), data["scan_mode"]


def read_battery(address):
    """Return Battery1 percentage without printing the private address."""
    try:
        from gi.repository import Gio
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        objects = bus.call_sync(
            "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects", None, None,
            Gio.DBusCallFlags.NO_AUTO_START, 5000, None).unpack()[0]
        for ifaces in objects.values():
            device = ifaces.get("org.bluez.Device1", {})
            if device.get("Address", "").upper() == address:
                battery = ifaces.get("org.bluez.Battery1", {})
                value = battery.get("Percentage")
                return int(value) if value is not None else None
    except Exception:
        # Battery1 is an optional BlueZ interface; charging can still be
        # guarded by the CPS identity and the kernel's electrical watchdog.
        return None
    return None


def touch_ready(controller=CONTROLLER):
    try:
        stats = fields((controller / "touch_stats").read_text())
        return (stats.get("enabled") == "1" and stats.get("panel_ready") == "1"
                and stats.get("suspended") == "0" and stats.get("start_error") == "0")
    except OSError:
        return False


def status_safe(status_text, attach_text):
    """Return (safe, reason), using conservative external limits."""
    status = fields(status_text)
    attach = fields(attach_text)
    if integer(status, "enabled", 0) and integer(status, "charge_disable", 1) != 0:
        return False, "charge-inhibit-not-cleared"
    if integer(attach, "removes", 0) or integer(attach, "stop_packets", 0):
        return False, "pen-removed-or-stopped"
    if integer(attach, "flags_seen", 0) & FAULT_FLAGS:
        return False, "charger-fault-flag"
    vin = integer(attach, "vin", 0)
    iin = integer(attach, "iin", 0)
    temperature = integer(attach, "temperature", 0)
    ept = integer(attach, "ept", 0)
    if vin and not 4000 <= vin <= 6500:
        return False, "voltage-limit"
    if iin > 250:
        return False, "current-limit"
    if temperature > 45:
        return False, "temperature-limit"
    if ept:
        return False, "ept"
    return True, "ok"


def read_state(path):
    try:
        value = json.loads(path.read_text())
        if (not isinstance(value, dict) or type(value.get("failures")) is not int
                or not 0 <= value["failures"] <= len(RETRY_DELAYS)):
            return {}
        return value
    except (OSError, ValueError):
        return {}


def write_state(path, state):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, sort_keys=True) + "\n")
    temporary.chmod(0o600)
    temporary.replace(path)


def run_writer(path):
    code = ("import os, sys\n"
            "fd = os.open(sys.argv[1], os.O_WRONLY)\n"
            "try:\n os.write(fd, b'charge\\n')\n"
            "finally:\n os.close(fd)\n")
    return subprocess.Popen([sys.executable, "-c", code, str(path)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def result_reason(reason, returncode, request_timed_out):
    if request_timed_out and reason == "ok":
        return "charge-request-timeout"
    if returncode == 0 and reason == "ok":
        return "completed"
    if returncode not in (0, None) and reason == "ok":
        return "charge-request-failed"
    return reason


def unload():
    result = subprocess.run(["rmmod", "caihong_pen_power"],
                            capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise ChargeError("module-unload-failed")


def attempt(module, stop_requested):
    subprocess.run(["insmod", str(module), "stage=2"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    process = None
    deadline = time.monotonic() + ATTEMPT_TIMEOUT
    reason = "completed"
    try:
        if not PROVIDER.exists():
            raise ChargeError("provider-missing")
        process = run_writer(PROVIDER / "attach_once")
        while process.poll() is None and time.monotonic() < deadline:
            if stop_requested():
                reason = "stop-requested"
                break
            try:
                safe, reason = status_safe(
                    (PROVIDER / "status").read_text(),
                    (PROVIDER / "attach_status").read_text())
            except OSError:
                safe, reason = False, "status-unavailable"
            if not safe:
                break
            time.sleep(0.25)
        request_timed_out = process.poll() is None
        if request_timed_out:
            stop_process(process)
        reason = result_reason(reason, process.returncode, request_timed_out)
        return reason, (PROVIDER / "status").read_text(), (PROVIDER / "attach_status").read_text()
    finally:
        if process is not None:
            stop_process(process)
        unload()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--state", type=Path, default=DEFAULT_STATE)
    parser.add_argument("--module", type=Path, default=DEFAULT_MODULE)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("run as root")
    address, mode = load_config(args.config)
    if mode != 1 or not args.module.exists():
        return 0
    state = read_state(args.state)
    now = time.monotonic()
    if now < float(state.get("retry_after", 0)):
        return 0
    battery = read_battery(address)
    if battery is not None and battery >= FULL_BATTERY:
        state.update(status="full", battery=battery, failures=0, retry_after=0)
        write_state(args.state, state)
        return 0
    if not touch_ready():
        state.update(status="waiting-for-touch", battery=battery)
        write_state(args.state, state)
        return 0
    stop_flag = {"value": False}
    signal.signal(signal.SIGTERM, lambda *_: stop_flag.update(value=True))
    signal.signal(signal.SIGINT, lambda *_: stop_flag.update(value=True))
    with LOCK.open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 0
        try:
            reason, status, attach = attempt(args.module, lambda: stop_flag["value"])
            data = fields(status)
            complete = integer(fields(attach), "charge_complete", 0) == 1
            success = reason == "completed" and integer(data, "result", -1) == 0 and complete
            if success:
                state.update(status="charging-observed", failures=0, retry_after=0,
                             battery=battery, charge_samples=integer(fields(attach), "charge_samples", 0))
            else:
                failures = min(state.get("failures", 0) + 1, len(RETRY_DELAYS))
                delay = RETRY_DELAYS[failures - 1]
                state.update(status=reason, failures=failures,
                             retry_after=time.monotonic() + delay, battery=battery)
            write_state(args.state, state)
        except (ChargeError, OSError, subprocess.SubprocessError) as error:
            failures = min(state.get("failures", 0) + 1, len(RETRY_DELAYS))
            state.update(status=type(error).__name__, failures=failures,
                         retry_after=time.monotonic() + RETRY_DELAYS[failures - 1], battery=battery)
            write_state(args.state, state)
            return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
