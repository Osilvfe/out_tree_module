#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Recovery policy tests: readiness, pairing, backoff, ACK and cancellation."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "recovery", Path(__file__).with_name("caihong-pen-autoconnect.py"))
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)
ADDRESS = "02:00:00:00:00:01"


class FakeController:
    def __init__(self, mode=-1, awake=True):
        self.mode, self.awake = mode, awake
        self.writes = []
        self.fail = False

    def ready(self):
        return self.awake

    def restore(self, mode):
        if self.fail:
            raise OSError("ACK failed")
        if self.mode == mode:
            return False
        self.writes.append(mode)
        self.mode = mode
        return True


class FakeBluez:
    def __init__(self, linked=True):
        self.linked = linked
        self.reason = None
        self.calls = []
        self.error = None
        self.complete_on_timeout = False

    def find(self, address):
        self.calls.append("find")
        return (None, self.reason) if self.reason else (("/pen", self.linked), None)

    def connected(self, path):
        return self.linked

    def connect(self, path):
        self.calls.append("connect")
        if self.error:
            self.linked = self.complete_on_timeout
            raise self.error
        self.linked = True

    def cancel(self, path):
        # Exercise production cancellation, including its late-success check.
        recovery.Bluez.cancel(self, path)

    def call(self, path, interface, method):
        self.calls.append(method)


class RecoveryTest(unittest.TestCase):
    def test_already_connected_restores_unknown_mode_once_without_connect(self):
        controller, bluez, state = FakeController(), FakeBluez(), {}
        self.assertEqual(recovery.reconcile(controller, bluez, ADDRESS, 1, state, 10),
                         ("connected", True))
        self.assertEqual(recovery.reconcile(controller, bluez, ADDRESS, 1, state, 20),
                         ("connected", False))
        self.assertEqual(controller.writes, [1])
        self.assertNotIn("connect", bluez.calls)

    def test_sleeping_or_missing_controller_does_not_touch_bluetooth(self):
        for controller in (None, FakeController(awake=False)):
            bluez = FakeBluez(False)
            self.assertEqual(recovery.reconcile(controller, bluez, ADDRESS, 1, {}, 0),
                             ("waiting-for-touch", False))
            self.assertEqual(bluez.calls, [])

    def test_missing_unpaired_blocked_or_radio_off_does_not_set_mode_or_connect(self):
        for reason in ("paired-device-unavailable", "pair-manually-first", "device-blocked", "adapter-off"):
            controller, bluez = FakeController(), FakeBluez(False)
            bluez.reason = reason
            self.assertEqual(recovery.reconcile(controller, bluez, ADDRESS, 1, {}, 0),
                             (reason, False))
            self.assertEqual(controller.writes, [])
            self.assertEqual(bluez.calls, ["find"])

    def test_bonded_reconnect_and_reset_failed_attempt_backoff(self):
        controller, bluez = FakeController(mode=1), FakeBluez(False)
        state = {"failures": 3, "retry_after": 0}
        self.assertEqual(recovery.reconcile(controller, bluez, ADDRESS, 1, state, 0),
                         ("connected", False))
        self.assertEqual(bluez.calls, ["find", "connect"])
        self.assertEqual(state, {"failures": 0, "retry_after": 0})

    def test_backoff_is_bounded_and_does_not_repeat_connect_early(self):
        controller, bluez, state = FakeController(mode=1), FakeBluez(False), {}
        bluez.error = recovery.BluezError("org.bluez.Error.Failed")
        now = 0
        for expected_delay in (30, 60, 120, 240, 300, 300):
            self.assertEqual(recovery.reconcile(controller, bluez, ADDRESS, 1, state, now)[0],
                             "connect-failed")
            self.assertEqual(state["retry_after"], now + expected_delay)
            count = bluez.calls.count("connect")
            self.assertEqual(recovery.reconcile(controller, bluez, ADDRESS, 1, state, now + 1)[0],
                             "waiting-to-retry")
            self.assertEqual(bluez.calls.count("connect"), count)
            now = state["retry_after"]

    def test_link_recovered_by_bluez_clears_backoff_immediately(self):
        state = {"failures": 5, "retry_after": 900}
        self.assertEqual(recovery.reconcile(FakeController(), FakeBluez(), ADDRESS, 1, state, 2)[0],
                         "connected")
        self.assertEqual(state["retry_after"], 0)

    def test_in_progress_from_another_request_is_not_cancelled(self):
        bluez = FakeBluez(False)
        bluez.error = recovery.BluezError("org.bluez.Error.InProgress")
        self.assertEqual(recovery.connect_once(bluez, "/pen"), "connect-pending")
        self.assertEqual(bluez.calls, ["connect"])

    def test_local_timeout_cancels_own_pending_connection(self):
        bluez = FakeBluez(False)
        bluez.error = recovery.BluezError("local-timeout", timed_out=True)
        self.assertEqual(recovery.connect_once(bluez, "/pen"), "connect-timeout")
        self.assertEqual(bluez.calls, ["connect", "Disconnect"])

    def test_connection_completed_at_timeout_is_preserved(self):
        bluez = FakeBluez(False)
        bluez.error = recovery.BluezError("local-timeout", timed_out=True)
        bluez.complete_on_timeout = True
        self.assertEqual(recovery.connect_once(bluez, "/pen"), "connected")
        self.assertEqual(bluez.calls, ["connect"])

    def test_mode_ack_failure_stops_before_connect(self):
        controller, bluez = FakeController(), FakeBluez(False)
        controller.fail = True
        with self.assertRaises(OSError):
            recovery.reconcile(controller, bluez, ADDRESS, 1, {}, 0)
        self.assertNotIn("connect", bluez.calls)


class BluezSelectionTest(unittest.TestCase):
    def test_only_one_exact_paired_unblocked_device_on_powered_adapter_is_used(self):
        adapter = {"org.bluez.Adapter1": {"Powered": True}}
        props = {"Address": ADDRESS, "Paired": True, "Connected": False, "Adapter": "/adapter"}
        objects = {"/adapter": adapter, "/pen": {"org.bluez.Device1": props}}
        backend = object.__new__(recovery.Bluez)
        backend.call = lambda *args: (objects,)
        self.assertEqual(backend.find(ADDRESS), (("/pen", False), None))
        self.assertEqual(backend.find("02:00:00:00:00:02")[1], "paired-device-unavailable")
        for key, value, expected in (("Paired", False, "pair-manually-first"),
                                     ("Blocked", True, "device-blocked")):
            old = props.get(key)
            props[key] = value
            self.assertEqual(backend.find(ADDRESS), (None, expected))
            props[key] = old
        adapter["org.bluez.Adapter1"]["Powered"] = False
        self.assertEqual(backend.find(ADDRESS), (None, "adapter-off"))
        adapter["org.bluez.Adapter1"]["Powered"] = True
        objects["/duplicate"] = {"org.bluez.Device1": dict(props)}
        self.assertEqual(backend.find(ADDRESS)[1], "paired-device-unavailable")


class FilesTest(unittest.TestCase):
    def test_config_requires_explicit_validated_mode_and_exact_address(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config.json"
            path.write_text(json.dumps({"address": ADDRESS, "scan_mode": 1}))
            self.assertEqual(recovery.load_config(path), (ADDRESS, 1))
            for mode in (True, 0, -1, 6, "1"):
                path.write_text(json.dumps({"address": ADDRESS, "scan_mode": mode}))
                with self.assertRaises(ValueError):
                    recovery.load_config(path)
            path.write_text(json.dumps({"address": "$(exit)", "scan_mode": 1}))
            with self.assertRaises(ValueError):
                recovery.load_config(path)

    def test_controller_discovery_readiness_and_unchanged_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            node = root / "spi0.0"
            node.mkdir()
            (node / "driver").symlink_to(root / "drivers" / "nt36532e")
            (node / "pen_scan").write_text("1\n")
            (node / "pen_stats").touch()
            stats = node / "touch_stats"
            stats.write_text("enabled=1 panel_ready=1 suspended=0 start_error=0")
            controller = recovery.Controller.discover(root)
            self.assertTrue(controller.ready())
            before = (node / "pen_scan").stat().st_mtime_ns
            self.assertFalse(controller.restore(1))
            self.assertEqual((node / "pen_scan").stat().st_mtime_ns, before)
            self.assertTrue(controller.restore(2))
            self.assertEqual((node / "pen_scan").read_text(), "2\n")
            stats.write_text("enabled=1 panel_ready=0 suspended=0 start_error=0")
            self.assertFalse(controller.ready())
            stats.write_text("enabled=1 panel_ready=1 suspended=0 start_error=-110")
            self.assertFalse(controller.ready())

    def test_state_survives_runs_but_invalid_retry_time_is_discarded(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "state.json"
            state = {"failures": 1, "retry_after": 0, "status": "connected"}
            recovery.write_state(path, state)
            self.assertEqual(recovery.read_state(path), state)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            state["retry_after"] = 10 ** 30
            recovery.write_state(path, state)
            self.assertEqual(recovery.read_state(path), {})


if __name__ == "__main__":
    unittest.main()
