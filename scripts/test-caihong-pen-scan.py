#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Powered-pen scan helper tests with fake sysfs and a virtual clock."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("pen_scan", Path(__file__).with_name("caihong-pen-scan.py"))
scan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scan)


class FakeController:
    def __init__(self, original=-1, working=2):
        self.mode = original
        self.working = working
        self.writes = []
        self.reports = 0
        self.packets = 0
        self.samples = 0
        self.static = False
        self.contact = True
        self.fail_mode = None
        self.restart = False
        self.reset_counter = False
        self.ready = True

    def set_mode(self, mode):
        self.writes.append(mode)
        self.samples = 0
        if mode == self.fail_mode:
            self.mode = -1
            raise OSError("ACK timeout")
        self.mode = mode

    def snapshot(self):
        self.samples += 1
        self.packets += 10  # Even empty/ID packets can increase this count.
        active = self.mode == self.working
        if active:
            self.reports += 5
        if self.reset_counter and self.samples == 3:
            self.reports = 0
        return {"touch": {"enabled": int(self.ready), "panel_ready": int(self.ready),
                          "suspended": 0, "start_error": 0, "spi_errors": 0,
                          "starts": 2 if self.restart and self.samples >= 3 else 1},
                "pen": {"scan_type": self.mode, "command_error": 0,
                        "packets": self.packets, "reports": self.reports,
                        "checksum_errors": 0, "out_of_range": 0, "unknown_formats": 0,
                        "format": 1 if active else 0xff, "in_range": int(active),
                        "contact": int(active and self.contact),
                        "pressure": 100 if active and self.contact else 0,
                        "raw_x": 100 if self.static else self.reports, "raw_y": 50}}


class ScanTest(unittest.TestCase):
    def run_fake(self, controller, modes=(1, 2, 3)):
        clock = [0.0]

        def sleep(duration):
            clock[0] += duration

        result = {"candidate": None, "trials": []}
        with patch.object(scan.time, "sleep", side_effect=sleep), \
                patch.object(scan.time, "monotonic", side_effect=lambda: clock[0]), \
                contextlib.redirect_stdout(io.StringIO()):
            scan.run_scan(controller, modes, 3, result)
        return result, clock[0]

    def test_keep_first_mode_with_fresh_moving_contact_reports(self):
        controller = FakeController()
        result, elapsed = self.run_fake(controller)
        self.assertEqual(result["candidate"], 2)
        self.assertEqual(controller.writes, [1, 2])
        self.assertEqual(result["after"]["pen"]["scan_type"], 2)
        self.assertEqual(result["trials"][1]["fresh_samples"], 3)
        self.assertEqual(result["trials"][1]["delta"]["reports"], 15)
        self.assertLess(elapsed, 5)
        self.assertNotIn("restored_mode", result)

    def test_empty_packets_do_not_match_and_unknown_mode_falls_back_to_off(self):
        controller = FakeController(working=5)
        result, elapsed = self.run_fake(controller)
        self.assertIsNone(result["candidate"])
        self.assertEqual(controller.writes, [1, 2, 3, 0])
        self.assertEqual(result["restored_mode"], 0)
        self.assertAlmostEqual(elapsed, 10.5)
        self.assertGreater(result["trials"][0]["delta"]["packets"], 0)
        self.assertEqual(result["trials"][0]["delta"]["reports"], 0)

    def test_static_or_hover_only_samples_do_not_claim_a_working_pen(self):
        for field in ("static", "contact"):
            controller = FakeController(original=4)
            setattr(controller, field, field == "static")
            result, _ = self.run_fake(controller)
            self.assertIsNone(result["candidate"])
            self.assertEqual(result["restored_mode"], 4)
            self.assertEqual(controller.writes, [1, 2, 3, 4])

    def test_failed_ack_stops_sweep_and_restores_known_mode(self):
        controller = FakeController(original=4)
        controller.fail_mode = 1
        result, _ = self.run_fake(controller)
        self.assertEqual(controller.writes, [1, 4])
        self.assertEqual(result["error"], "ACK timeout")
        self.assertEqual(result["restored_mode"], 4)
        self.assertIsNone(result["candidate"])

    def test_failed_restore_remains_explicit_and_does_not_claim_restored(self):
        controller = FakeController(original=4, working=5)
        controller.fail_mode = 4
        result, _ = self.run_fake(controller)
        self.assertIn("restore_error", result)
        self.assertNotIn("restored_mode", result)
        self.assertEqual(result["after"]["pen"]["scan_type"], -1)

    def test_interruption_restores_mode(self):
        controller = FakeController(original=3)
        with patch.object(scan, "observe", side_effect=KeyboardInterrupt):
            result, _ = self.run_fake(controller)
        self.assertEqual(result["error"], "Interrupted")
        self.assertEqual(controller.writes, [1, 3])
        self.assertEqual(result["restored_mode"], 3)

    def test_sleeping_controller_receives_no_command(self):
        controller = FakeController()
        controller.ready = False
        with self.assertRaisesRegex(RuntimeError, "not awake/ready"):
            self.run_fake(controller)
        self.assertEqual(controller.writes, [])

    def test_restart_or_counter_reset_aborts_even_if_coordinates_arrive(self):
        for failure, message in (("restart", "restarted"), ("reset_counter", "counters reset")):
            controller = FakeController(original=4, working=1)
            setattr(controller, failure, True)
            result, _ = self.run_fake(controller)
            self.assertIsNone(result["candidate"])
            self.assertIn(message, result["error"])
            self.assertEqual(controller.writes, [1, 4])

    def test_parse_hex_formats_decimal_counts_and_unknown_mode(self):
        self.assertEqual(scan.parse_stats("scan_type=-1 format=ff reports=10 fw=15 protocol=01"),
                         {"scan_type": -1, "format": 255, "reports": 10, "fw": 21, "protocol": 1})


if __name__ == "__main__":
    unittest.main()
