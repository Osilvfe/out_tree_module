#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Restore an explicitly configured, already paired pen and its scan mode.

Run once from a systemd timer. Uses python-gobject/Gio for typed BlueZ calls;
does not discover, pair, trust, power adapters, or access the CPS charger.
Shares the scan diagnostic's lock. Stop the timer before changing scan modes.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import time


class BluezError(RuntimeError):
    def __init__(self, code, timed_out=False):
        super().__init__(code)
        self.timed_out = timed_out


def load_config(path):
    data = json.loads(path.read_text())
    if (not isinstance(data, dict) or set(data) != {"address", "scan_mode"}
            or not isinstance(data["address"], str)
            or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", data["address"])
            or type(data["scan_mode"]) is not int or data["scan_mode"] not in range(1, 6)):
        raise ValueError("Expected address and a previously validated scan_mode (1-5)")
    return data["address"].upper(), data["scan_mode"]


class Controller:
    def __init__(self, path):
        self.path = path

    @classmethod
    def discover(cls, root=Path("/sys/bus/spi/devices")):
        matches = [node for node in root.glob("spi*.*")
                   if (node / "driver").resolve().name == "nt36532e"
                   and all((node / field).exists()
                           for field in ("pen_scan", "touch_stats", "pen_stats"))]
        return cls(matches[0]) if len(matches) == 1 else None

    def ready(self):
        stats = dict(field.split("=", 1)
                     for field in (self.path / "touch_stats").read_text().split())
        return (stats.get("enabled") == "1" and stats.get("panel_ready") == "1"
                and stats.get("suspended") == "0" and stats.get("start_error") == "0")

    def restore(self, mode):
        control = self.path / "pen_scan"
        if int(control.read_text()) == mode:
            return False
        control.write_text(f"{mode}\n")
        if int(control.read_text()) != mode:
            raise OSError("Scan mode acknowledgement mismatch")
        return True


class Bluez:
    def __init__(self):
        from gi.repository import Gio, GLib
        self.Gio, self.GLib = Gio, GLib
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)

    def call(self, path, interface, method, parameters=None, timeout=5000):
        try:
            result = self.bus.call_sync(
                "org.bluez", path, interface, method, parameters, None,
                self.Gio.DBusCallFlags.NO_AUTO_START, timeout, None)
            return result.unpack()
        except self.GLib.Error as error:
            name = self.Gio.DBusError.get_remote_error(error)
            timed_out = (error.matches(self.Gio.io_error_quark(), self.Gio.IOErrorEnum.TIMED_OUT)
                         or name in ("org.freedesktop.DBus.Error.NoReply",
                                     "org.freedesktop.DBus.Error.Timeout"))
            # Never print a D-Bus error's free-form text: it can contain addresses.
            raise BluezError(name or "local-dbus-error", timed_out) from None

    def find(self, address):
        objects = self.call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")[0]
        matches = [(path, ifaces["org.bluez.Device1"]) for path, ifaces in objects.items()
                   if ifaces.get("org.bluez.Device1", {}).get("Address", "").upper() == address]
        if len(matches) != 1:
            return None, "paired-device-unavailable"
        path, props = matches[0]
        if not props.get("Paired"):
            return None, "pair-manually-first"
        if props.get("Blocked"):
            return None, "device-blocked"
        adapter = objects.get(props.get("Adapter"), {}).get("org.bluez.Adapter1", {})
        if not adapter.get("Powered"):
            return None, "adapter-off"
        return (path, props.get("Connected", False)), None

    def connected(self, path):
        args = self.GLib.Variant("(ss)", ("org.bluez.Device1", "Connected"))
        return self.call(path, "org.freedesktop.DBus.Properties", "Get", args)[0]

    def connect(self, path):
        self.call(path, "org.bluez.Device1", "Connect", timeout=20000)

    def cancel(self, path):
        # Device1.Disconnect also cancels a pending Connect request.
        # Preserve a link which completed at the timeout boundary.
        if not self.connected(path):
            self.call(path, "org.bluez.Device1", "Disconnect")


def connect_once(bluez, path):
    try:
        print("Reconnecting configured paired pen", flush=True)
        bluez.connect(path)
    except BluezError as error:
        if not error.timed_out:
            # InProgress belongs to another request; do not cancel it.
            return "connect-pending" if str(error).endswith(".InProgress") else "connect-failed"
        bluez.cancel(path)
        return "connected" if bluez.connected(path) else "connect-timeout"
    except KeyboardInterrupt:
        bluez.cancel(path)
        raise
    return "connected" if bluez.connected(path) else "connect-incomplete"


def reconcile(controller, bluez, address, mode, state, now):
    if controller is None or not controller.ready():
        return "waiting-for-touch", False
    device, reason = bluez.find(address)
    if device is None:
        return reason, False
    # Configured mode is authoritative, but never write during a diagnostic.
    # The caller holds /run/lock/caihong-pen-scan.lock throughout this run.
    restored = controller.restore(mode)
    path, connected = device
    if connected:
        state.update(failures=0, retry_after=0)
        return "connected", restored
    if now < state.get("retry_after", 0):
        return "waiting-to-retry", restored
    result = connect_once(bluez, path)
    if result == "connected":
        state.update(failures=0, retry_after=0)
    else:
        failures = min(state.get("failures", 0) + 1, 5)
        delay = min(30 * 2 ** (failures - 1), 300)
        state.update(failures=failures, retry_after=now + delay)
    return result, restored


def read_state(path):
    try:
        state = json.loads(path.read_text())
        if (not isinstance(state, dict)
                or type(state.get("failures")) is not int or not 0 <= state["failures"] <= 5
                or type(state.get("retry_after")) not in (int, float)
                or not 0 <= state["retry_after"] <= time.monotonic() + 360):
            return {}
        return state
    except (FileNotFoundError, ValueError):
        return {}


def write_state(path, state):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(state) + "\n")
    tmp.chmod(0o600)
    tmp.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path("/etc/caihong-pen.json"))
    parser.add_argument("--state", type=Path,
                        default=Path("/run/caihong-pen-autoconnect/state.json"))
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("Run as root for the pen_scan interface")
    address, mode = load_config(args.config)
    with Path("/run/lock/caihong-pen-scan.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 0
        state = read_state(args.state)
        previous = state.get("status")
        state.setdefault("failures", 0)
        state.setdefault("retry_after", 0)
        try:
            status, restored = reconcile(Controller.discover(), Bluez(), address, mode,
                                         state, time.monotonic())
        except (BluezError, OSError) as error:
            # Keep failures visible without publishing a pen address.
            status, restored = f"retry-later-{type(error).__name__}", False
        state["status"] = status
        write_state(args.state, state)
        if restored:
            print(f"Restored configured pen scan mode {mode}", flush=True)
        if status != previous:
            print(f"Pen recovery: {status}", flush=True)
    return 0


if __name__ == "__main__":
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
    except (OSError, ValueError, ImportError):
        raise SystemExit("Invalid/unreadable pen configuration, runtime state, or missing python-gobject")
