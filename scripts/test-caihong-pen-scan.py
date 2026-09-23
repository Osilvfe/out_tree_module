#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Powered-pen scan helper tests with fake sysfs and a virtual clock."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
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


class SummaryTest(unittest.TestCase):
    def log(self):
        before = FakeController(original=1, working=5).snapshot()
        before["touch"].update({"irq": 100, "reads": 100, "frames": 100,
                                "contacts": 0, "boot_events": 0})
        return {"version": 1, "candidate": None, "restored_mode": 0,
                "trials": [{"mode": 1, "candidate": False, "before": before,
                            "samples": [copy.deepcopy(before)]}]}

    def test_no_irq_event_reads_differ_from_empty_pen_packets(self):
        result = self.log()
        row = scan.summarize(result)[0]
        self.assertTrue(row["ack_observed"])
        self.assertEqual(row["evidence"], "no_new_event_reads")
        self.assertEqual(row["delta"]["touch"]["reads"], 0)
        sample = result["trials"][0]["samples"][0]
        sample["touch"]["irq"] += 10
        sample["touch"]["reads"] += 10
        sample["pen"]["packets"] += 10
        self.assertEqual(scan.summarize(result)[0]["evidence"], "events_without_pen_coordinates")
        sample["pen"]["reports"] += 4
        self.assertEqual(scan.summarize(result)[0]["evidence"], "pen_coordinates_seen")

    def test_boot_events_do_not_imply_pen_decoder_received_packets(self):
        result = self.log()
        sample = result["trials"][0]["samples"][0]
        sample["touch"]["reads"] += 2
        sample["touch"]["boot_events"] += 2
        self.assertEqual(scan.summarize(result)[0]["evidence"], "events_read_without_pen_dispatch")
        sample["touch"]["spi_errors"] += 1
        self.assertEqual(scan.summarize(result)[0]["evidence"], "controller_restart_or_spi_error")

    def test_partial_failed_missing_and_reset_data_remain_inconclusive(self):
        for failure in ("partial", "ack", "missing", "reset"):
            result = self.log()
            trial = result["trials"][0]
            if failure == "partial":
                del trial["candidate"]
            elif failure == "ack":
                trial["before"]["pen"]["command_error"] = -110
            elif failure == "missing":
                del trial["before"]["touch"]["reads"]
            else:
                trial["samples"][0]["touch"]["reads"] = 1
            row = scan.summarize(result)[0]
            self.assertEqual(row["evidence"], "incomplete_or_changed_state")
            if failure == "missing":
                self.assertIsNone(row["delta"]["touch"]["reads"])
            if failure == "reset":
                self.assertEqual(row["counter_resets"], ["touch.reads"])

    def test_offline_cli_does_not_access_hardware_require_root_or_write(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pen-scan-example.json"
            saved = json.dumps(self.log())
            path.write_text(saved)
            with patch("sys.argv", ["pen-scan", "--summarize", str(path)]), \
                    patch.object(scan, "discover", side_effect=AssertionError("hardware access")), \
                    patch.object(scan.os, "geteuid", side_effect=AssertionError("root check")), \
                    patch.object(scan.fcntl, "flock", side_effect=AssertionError("lock write")), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(scan.main(), 0)
            self.assertIn("ack=observed irq=0 reads=0", output.getvalue())
            self.assertIn("no_new_event_reads", output.getvalue())
            self.assertEqual(path.read_text(), saved)
            self.assertEqual(list(Path(directory).iterdir()), [path])


if __name__ == "__main__":
    unittest.main()
