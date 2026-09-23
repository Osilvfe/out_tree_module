#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test a powered pen using the existing NT36532E pen_scan interface.

Only changes the documented touchscreen scan mode. No CPS/I2C, GPIO, HBOOST,
firmware-flash or Bluetooth commands. A candidate mode is kept for evtest;
otherwise restore the prior known mode, or disable scanning if it was -1.
Python standard library only. Keep the display awake and draw with the pen.
--summarize reads an existing JSON log without accessing hardware.
"""

import argparse
from datetime import datetime
import fcntl
import json
import os
from pathlib import Path
import signal
import time


MODES = {1: "Havon", 2: "Maxeye", 3: "Maxeye 2nd", 4: "Sunwoda", 5: "Maxeye 3rd"}
PEN_COUNTS = ("packets", "reports", "checksum_errors", "out_of_range", "unknown_formats")
HEX_FIELDS = {"format", "fw", "protocol"}
TOUCH_COUNTS = ("irq", "reads", "frames", "contacts", "spi_errors", "boot_events", "starts")


def parse_stats(text):
    result = {}
    for field in text.split():
        key, value = field.split("=", 1)
        result[key] = int(value, 16 if key in HEX_FIELDS else 10)
    return result


class Controller:
    def __init__(self, path):
        self.path = path

    def snapshot(self):
        return {"touch": parse_stats((self.path / "touch_stats").read_text()),
                "pen": parse_stats((self.path / "pen_stats").read_text())}

    def set_mode(self, mode):
        (self.path / "pen_scan").write_text(f"{mode}\n")
        applied = int((self.path / "pen_scan").read_text())
        if applied != mode:
            raise RuntimeError(f"Requested scan mode {mode}, driver reports {applied}")


def discover(root=Path("/sys/bus/spi/devices")):
    matches = [node for node in root.glob("spi*.*")
               if all((node / name).exists() for name in ("touch_stats", "pen_stats", "pen_scan"))
               and (node / "driver").resolve().name == "nt36532e"]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one bound NT36532E with pen diagnostics, found {len(matches)}")
    return Controller(matches[0])


def check_ready(snapshot, starts=None):
    touch = snapshot["touch"]
    pen = snapshot["pen"]
    if (touch["enabled"] != 1 or touch["panel_ready"] != 1 or touch["suspended"] != 0
            or touch["start_error"] != 0):
        raise RuntimeError("Touch controller is not awake/ready; keep the display on")
    if starts is not None and touch["starts"] != starts:
        raise RuntimeError("Touch controller restarted during the test; stopping the scan sweep")
    for name in PEN_COUNTS + ("scan_type", "command_error", "format", "in_range",
                              "contact", "pressure", "raw_x", "raw_y"):
        if name not in pen:
            raise RuntimeError(f"Missing pen diagnostic field: {name}")


def observe(controller, mode, seconds, trial, starts):
    # Exclude packets already queued around the previous protocol change.
    time.sleep(0.5)
    before = controller.snapshot()
    check_ready(before, starts)
    if before["pen"]["scan_type"] != mode or before["pen"]["command_error"]:
        raise RuntimeError("Pen scan command was not acknowledged or mode changed")
    trial["before"] = before
    previous = before
    fresh = 0
    positions = set()
    contact = False
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        time.sleep(min(0.2, max(0, seconds - (time.monotonic() - start))))
        current = controller.snapshot()
        trial["samples"].append({"elapsed": round(time.monotonic() - start, 3), **current})
        check_ready(current, starts)
        pen = current["pen"]
        if pen["scan_type"] != mode or pen["command_error"]:
            raise RuntimeError("Pen scan state changed during observation")
        if any(pen[key] < previous["pen"][key] for key in PEN_COUNTS):
            raise RuntimeError("Pen counters reset during observation")
        if current["touch"]["spi_errors"] != before["touch"]["spi_errors"]:
            raise RuntimeError("Touch SPI errors changed during observation")
        if pen["reports"] > previous["pen"]["reports"] and pen["format"] == 1 and pen["in_range"]:
            fresh += 1
            positions.add((pen["raw_x"], pen["raw_y"]))
            contact |= bool(pen["contact"] and pen["pressure"] > 0)
        previous = current
        trial["delta"] = {key: pen[key] - before["pen"][key] for key in PEN_COUNTS}
        trial["fresh_samples"] = fresh
        trial["distinct_positions"] = len(positions)
        trial["contact_seen"] = contact
        # No-pen/ID packets and a static cached point cannot identify a mode.
        if trial["delta"]["reports"] >= 5 and fresh >= 3 and len(positions) >= 2 and contact:
            trial["candidate"] = True
            return True
    trial["candidate"] = False
    return False


def run_scan(controller, modes, seconds, result):
    before = controller.snapshot()
    result["before"] = before
    check_ready(before)
    original = before["pen"]["scan_type"]
    if original not in (-1, 0, *MODES):
        raise RuntimeError(f"Unexpected original scan mode: {original}")
    restore = original if original >= 0 else 0
    result["restore_mode_if_no_candidate"] = restore
    if original == -1:
        print("初始扫描模式未知；若未找到候选或测试失败，会设置为 0（关闭笔扫描）。", flush=True)
    changed = False
    try:
        for mode in modes:
            print(f"测试模式 {mode} ({MODES[mode]})，最多 {seconds:g} 秒："
                  "请用笔持续画线、轻点屏幕。", flush=True)
            trial = {"mode": mode, "name": MODES[mode], "samples": []}
            result["trials"].append(trial)
            changed = True  # Even a failed write may have reached the controller.
            controller.set_mode(mode)
            if observe(controller, mode, seconds, trial, before["touch"]["starts"]):
                result["candidate"] = mode
                break
    except (OSError, RuntimeError, ValueError, KeyError, KeyboardInterrupt) as error:
        result["error"] = str(error) or "Interrupted"
        result["candidate"] = None
    finally:
        if changed and result.get("candidate") is None:
            try:
                controller.set_mode(restore)
                result["restored_mode"] = restore
            except (OSError, RuntimeError, ValueError) as error:
                result["restore_error"] = str(error)
        try:
            result["after"] = controller.snapshot()
        except (OSError, ValueError) as error:
            result["after_error"] = str(error)


def summarize(result):
    if not isinstance(result, dict) or result.get("version") != 1 or not isinstance(result.get("trials"), list):
        raise ValueError("Not a version-1 pen scan log")
    rows = []
    for trial in result["trials"]:
        if not isinstance(trial, dict) or not isinstance(trial.get("samples"), list):
            raise ValueError("Invalid pen scan trial")
        snapshots = ([trial["before"]] if "before" in trial else []) + trial["samples"]
        if any(not isinstance(s, dict) or
               any(not isinstance(s.get(group), dict) for group in ("touch", "pen"))
               for s in snapshots):
            raise ValueError("Invalid touch/pen snapshot")
        delta = {}
        resets = []
        for group, keys in (("touch", TOUCH_COUNTS), ("pen", PEN_COUNTS)):
            delta[group] = {}
            for key in keys:
                values = [s[group].get(key) for s in snapshots]
                if len(values) < 2 or any(value is None for value in values):
                    delta[group][key] = None
                    continue
                if any(type(value) is not int or value < 0 for value in values):
                    raise ValueError(f"Invalid counter {group}.{key}")
                if any(right < left for left, right in zip(values, values[1:])):
                    resets.append(f"{group}.{key}")
                delta[group][key] = values[-1] - values[0]
        before = trial.get("before", {}).get("pen", {})
        mode = trial.get("mode")
        if type(mode) is not int or mode not in MODES:
            raise ValueError("Invalid scan mode in log")
        ack = before.get("scan_type") == mode and before.get("command_error") == 0
        stable = bool(snapshots) and all(
            s["touch"].get("enabled") == 1 and s["touch"].get("panel_ready") == 1
            and s["touch"].get("suspended") == 0 and s["touch"].get("start_error") == 0
            and s["pen"].get("scan_type") == mode and s["pen"].get("command_error") == 0
            for s in snapshots)
        complete = "before" in trial and bool(trial["samples"]) and "candidate" in trial
        touch, pen = delta["touch"], delta["pen"]
        if (not ack or not stable or not complete or resets or
                any(value is None for group in delta.values() for value in group.values())):
            evidence = "incomplete_or_changed_state"
        elif touch["starts"] or touch["spi_errors"]:
            evidence = "controller_restart_or_spi_error"
        elif pen["reports"] > 0:
            evidence = "pen_coordinates_seen"
        elif pen["packets"] > 0:
            evidence = "events_without_pen_coordinates"
        elif touch["reads"] > 0:
            evidence = "events_read_without_pen_dispatch"
        else:
            evidence = "no_new_event_reads"
        rows.append({"mode": mode, "ack_observed": ack, "complete": complete,
                     "delta": delta, "counter_resets": resets, "evidence": evidence})
    return rows


def print_summary(result):
    print("各模式观察期间的计数增量（? 表示日志缺项，不代表 0）：")
    for row in summarize(result):
        touch, pen = row["delta"]["touch"], row["delta"]["pen"]
        fields = [("irq", touch["irq"]), ("reads", touch["reads"]),
                  ("touch_frames", touch["frames"]), ("spi_errors", touch["spi_errors"]),
                  ("boot_events", touch["boot_events"]), ("pen_packets", pen["packets"]),
                  ("pen_reports", pen["reports"]), ("pen_checksum_errors", pen["checksum_errors"])]
        values = " ".join(f"{key}={value if value is not None else '?'}" for key, value in fields)
        print(f"模式 {row['mode']}: ack={'observed' if row['ack_observed'] else 'unknown'} "
              f"{values} evidence={row['evidence']}")
        if row["counter_resets"]:
            print("counter_resets=" + ",".join(row["counter_resets"]))
    for key in ("candidate", "error", "restore_error", "after_error", "restored_mode"):
        if key in result:
            print(f"{key}={result[key]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--sweep", action="store_true", help="test documented modes 1–5")
    group.add_argument("--type", type=int, choices=MODES, help="test one known vendor mode")
    group.add_argument("--summarize", type=Path, nargs="?", const=Path("latest"), metavar="FILE",
                       help="read a saved log only; defaults to the newest pen-scan-*.json here")
    parser.add_argument("--seconds", type=float, default=8, help="seconds per mode (3–15; default 8)")
    parser.add_argument("--output", type=Path,
                        default=Path(datetime.now().strftime("pen-scan-%Y%m%d-%H%M%S.json")))
    args = parser.parse_args()
    if args.summarize is not None:
        log = args.summarize
        if log == Path("latest"):
            logs = list(Path.cwd().glob("pen-scan-*.json"))
            if not logs:
                parser.error("No pen-scan-*.json here; supply the saved JSON path with --summarize")
            log = max(logs, key=lambda path: path.stat().st_mtime_ns)
        print(f"读取已有日志：{log.resolve()}")
        print_summary(json.loads(log.read_text()))
        return 0
    if not 3 <= args.seconds <= 15:
        parser.error("--seconds must be between 3 and 15")
    if os.geteuid() != 0:
        parser.error("Run with sudo to use the driver's pen_scan control")
    controller = discover()
    result = {"version": 1, "device": str(controller.path), "candidate": None, "trials": []}
    # Serializes this helper's runs; external manual sysfs writes are detected
    # by checking scan_type and command_error during each observation.
    with Path("/run/lock/caihong-pen-scan.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with args.output.open("x", encoding="utf-8") as output:
            try:
                run_scan(controller, list(MODES) if args.sweep else [args.type], args.seconds, result)
            except (OSError, RuntimeError, ValueError, KeyError, KeyboardInterrupt) as error:
                result["error"] = str(error) or "Interrupted"
            json.dump(result, output, ensure_ascii=False, indent=2)
            output.write("\n")
    if result.get("candidate") is not None:
        print(f"候选模式 {result['candidate']} 已保留：检测到持续坐标变化和笔尖压力。"
              "请用 evtest 的 Novatek NT36532E Pen 验证悬停、压力和离开屏幕。")
    else:
        print("未确认可用模式；这不能单独区分笔唤醒/连接、扫描协议和触控上报问题。")
    print_summary(result)
    print(f"完整记录：{args.output.resolve()}")
    if any(key in result for key in ("error", "restore_error", "after_error")):
        return 1
    return 0 if result["candidate"] is not None else 2


if __name__ == "__main__":
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit(str(error))
