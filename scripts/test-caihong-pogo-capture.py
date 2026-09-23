#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic event timing and evdev lifetime tests; never opens real input devices."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "capture", Path(__file__).with_name("caihong-pogo-capture.py"))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def record(kind, timestamp, event_type, code, value):
    return [kind, timestamp, event_type, code, value, timestamp + 0.001]


def motion(timestamp):
    return [record("touchpad", timestamp, 3, 53, 200),
            record("touchpad", timestamp, 0, 0, 0)]


class CaptureTest(unittest.TestCase):
    def test_raw_motion_stall_and_release_are_reported_separately(self):
        events = []
        for stamp in (0.01, 0.02, 0.03, 1.00, 1.01, 1.21, 1.22):
            events.extend(motion(stamp))
        events.extend([record("keyboard", 1.015, 1, 30, 1),
                       record("keyboard", 1.020, 1, 30, 0),
                       record("touchpad", 1.025, 1, 330, 0)])
        result = {"events": events[6:] + events[:6],
                  "phases": [{"name": "baseline", "start": 0, "end": 0.1},
                             {"name": "A", "start": 1, "end": 1.3}],
                  "status_before": {"rx_bytes": 100, "crc_errors": 0, "tx_frames": 3},
                  "status_after": {"rx_bytes": 120, "crc_errors": 2, "tx_frames": 3}}
        # Device batches can arrive out of timestamp order, but each batch
        # retains its internal order (including SYN_REPORT after ABS changes).
        summary = capture.analyze(result)
        self.assertEqual(summary["phases"][0]["motion"]["max_gap_ms"], 10)
        self.assertEqual(summary["phases"][1]["motion"]["max_gap_ms"], 200)
        self.assertEqual(summary["key_edges"][0]["next_motion_ms"], 195)
        self.assertEqual(summary["key_edges"][0]["max_motion_gap_near_edge_ms"], 200)
        self.assertEqual(summary["key_edges"][0]["touch_releases_next_500ms"], 1)
        self.assertEqual(summary["counter_delta"],
                         {"rx_bytes": 20, "crc_errors": 2, "tx_frames": 0})
        self.assertTrue(summary["complete_without_evdev_overflow"])

    def test_continuous_raw_motion_across_key_is_not_reported_as_stall(self):
        events = [record("keyboard", 0.505, 1, 62, 1)]
        for index in range(101):
            events.extend(motion(index / 100))
        result = {"events": events, "phases": [], "status_before": {}, "status_after": {}}
        summary = capture.analyze(result)
        self.assertEqual(summary["key_edges"][0]["next_motion_ms"], 5)
        self.assertEqual(summary["key_edges"][0]["max_motion_gap_near_edge_ms"], 10)
        self.assertEqual(summary["key_edges"][0]["touch_releases_next_500ms"], 0)

    def test_missing_events_overflow_and_counter_reset_are_not_success(self):
        result = {"events": [record("keyboard", 1, 1, 59, 1),
                              record("touchpad", 1.1, 0, 3, 0)],
                  "phases": [{"name": "F1", "start": 0, "end": 2}],
                  "status_before": {"touch_frames": 100},
                  "status_after": {"touch_frames": 1}}
        summary = capture.analyze(result)
        self.assertFalse(summary["complete_without_evdev_overflow"])
        self.assertIsNone(summary["key_edges"][0]["next_motion_ms"])
        self.assertIsNone(summary["phases"][0]["motion"]["max_gap_ms"])
        self.assertEqual(summary["counter_resets"], ["touch_frames"])
        self.assertNotIn("touch_frames", summary["counter_delta"])
        result["events"] = []
        result["error"] = "disconnected"
        self.assertFalse(capture.analyze(result)["complete_without_evdev_overflow"])

    def test_decode_native_time_and_only_test_keys(self):
        wire = b"".join(capture.EVENT.pack(100, micros, kind, code, value)
                        for micros, kind, code, value in [
                            (5000, 1, 30, 1), (6000, 1, 48, 1),
                            (7000, 4, 4, 0x70005), (8000, 0, 3, 0)])
        decoded = capture.decode(wire, "keyboard", 100, 100.010)
        self.assertEqual(len(decoded), 2)
        self.assertEqual(decoded[0][1:], [0.005, 1, 30, 1, 0.01])
        self.assertEqual(decoded[1][2:5], [0, 3, 0])
        self.assertEqual(capture.decode(wire, "keyboard", 101, 101), [])
        with self.assertRaisesRegex(RuntimeError, "Partial input_event"):
            capture.decode(wire[:-1], "touchpad", 100, 100.01)

    def test_fd_closes_if_clock_grab_or_capture_fails(self):
        for fail_at in (1, 2, 3):
            calls = []

            def ioctl(fd, command, value):
                calls.append(command)
                if len(calls) == fail_at:
                    raise OSError("ioctl failure")

            with patch.object(capture.os, "open", return_value=91), \
                    patch.object(capture.os, "close") as close, \
                    patch.object(capture.fcntl, "ioctl", side_effect=ioctl):
                with self.assertRaises((OSError, KeyboardInterrupt)):
                    with capture.open_events("fake", grab=True):
                        raise KeyboardInterrupt
                close.assert_called_once_with(91)
                self.assertEqual(calls[0], capture.EVIOCSCLOCKID)

    def test_discovery_requires_exact_unambiguous_devices(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, name in enumerate(("OnePlus Pogo Keyboard", "OnePlus Pogo Touchpad",
                                           "Novatek NT36532E Touchscreen")):
                device = root / f"event{index}" / "device"
                device.mkdir(parents=True)
                (device / "name").write_text(name + "\n")
            self.assertEqual(capture.discover_inputs(root),
                             {"keyboard": "/dev/input/event0", "touchpad": "/dev/input/event1"})
            (root / "event2/device/name").write_text("OnePlus Pogo Keyboard")
            with self.assertRaisesRegex(RuntimeError, "found 2"):
                capture.discover_inputs(root)

    def test_capture_exits_automatically_and_keeps_touchpad_ungrabbed(self):
        clock = [0.0]
        opened, closed = [], []

        @contextlib.contextmanager
        def fake_open(path, grab=False):
            opened.append((path, grab))
            try:
                yield len(opened)
            finally:
                closed.append(path)

        def fake_select(readers, writers, errors, timeout):
            clock[0] += timeout
            return [], [], []

        result = {"events": [], "phases": []}
        with patch.object(capture, "open_events", side_effect=fake_open), \
                patch.object(capture.time, "monotonic", side_effect=lambda: clock[0]), \
                patch.object(capture.select, "select", side_effect=fake_select), \
                contextlib.redirect_stdout(io.StringIO()):
            capture.capture({"keyboard": "kbd", "touchpad": "tp"}, result)
        self.assertEqual(clock[0], 25)
        self.assertEqual(opened, [("tp", False), ("kbd", True)])
        self.assertEqual(closed, ["kbd", "tp"])
        self.assertEqual([phase["name"] for phase in result["phases"]],
                         ["baseline", "A", "F1", "F4"])


if __name__ == "__main__":
    unittest.main()
