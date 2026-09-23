#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Capture raw pogo motion/key timing on the existing image; stdlib only.

Temporarily grabs only the keyboard. Closing its fd restores desktop input.
Reads existing driver counters; sends no UART, GPIO or power commands.
Only A/F1/F4 keyboard events are retained. Touchpad events are retained raw.
"""

import argparse
from bisect import bisect_left
from contextlib import contextmanager, ExitStack
from datetime import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import select
import signal
import statistics
import struct
import time


EVENT = struct.Struct("@llHHi")  # Native Linux input_event (64-bit on Caihong).
EVIOCGRAB = 0x40044590
EVIOCSCLOCKID = 0x400445A0
TEST_KEYS = {30: "A", 59: "F1", 62: "F4"}
PHASES = [("baseline", 4), ("A", 7), ("F1", 7), ("F4", 7)]
COUNTERS = ("rx_bytes", "candidates", "valid_frames", "crc_errors",
            "framing_errors", "tx_frames", "tx_errors", "touch_frames",
            "touch_contacts")
STATES = ("baud", "touchpad_disabled", "touchpad_target_enabled",
          "touchpad_restore_left", "touchpad_restore_error")


def discover_inputs(root=Path("/sys/class/input")):
    result = {}
    for kind, name in (("keyboard", "OnePlus Pogo Keyboard"),
                       ("touchpad", "OnePlus Pogo Touchpad")):
        matches = []
        for node in sorted(root.glob("event*")):
            try:
                if (node / "device/name").read_text().strip() == name:
                    matches.append(str(Path("/dev/input") / node.name))
            except OSError:
                continue
        if len(matches) != 1:
            raise RuntimeError(f"Expected one {name}, found {len(matches)}: {matches}")
        result[kind] = matches[0]
    return result


def status_snapshot():
    nodes = list(Path("/sys/bus/serial/drivers/oneplus-pogo").glob("*/status"))
    if len(nodes) != 1:
        return {"error": f"Expected one pogo status node, found {len(nodes)}"}
    try:
        # Deliberately omit last_rx: it may contain keyboard identity bytes.
        values = dict(re.findall(r"\b([a-z_]+)=(-?\d+)\b", nodes[0].read_text()))
        return {key: int(values[key]) for key in COUNTERS + STATES if key in values}
    except OSError as error:
        return {"error": str(error)}


@contextmanager
def open_events(path, grab=False):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        # A shared clock is essential when comparing events from two fds.
        fcntl.ioctl(fd, EVIOCSCLOCKID, struct.pack("i", time.CLOCK_MONOTONIC))
        if grab:
            fcntl.ioctl(fd, EVIOCGRAB, 1)
        yield fd
    finally:
        os.close(fd)  # Also releases EVIOCGRAB on failure or interruption.


def decode(data, kind, start, received):
    if len(data) % EVENT.size:
        raise RuntimeError("Partial input_event read; timing capture is incomplete")
    records = []
    for sec, usec, event_type, code, value in EVENT.iter_unpack(data):
        stamp = sec + usec / 1_000_000 - start
        if stamp < 0:
            continue  # Events queued before capture started.
        if kind == "keyboard" and not (
                (event_type == 1 and code in TEST_KEYS) or
                (event_type == 0 and code == 3)):  # SYN_DROPPED must survive filtering.
            continue
        records.append([kind, round(stamp, 6), event_type, code, value,
                        round(received - start, 6)])
    return records


def capture(devices, result):
    with ExitStack() as stack:
        touchpad = stack.enter_context(open_events(devices["touchpad"]))
        keyboard = stack.enter_context(open_events(devices["keyboard"], grab=True))
        kinds = {touchpad: "touchpad", keyboard: "keyboard"}
        start = time.monotonic()
        result["keyboard_grabbed"] = True
        for name, duration in PHASES:
            begin = time.monotonic()
            phase = {"name": name, "start": begin - start}
            result["phases"].append(phase)
            if name == "baseline":
                print("基线 4 秒：持续在触摸板中央画圈，暂不按键。", flush=True)
            else:
                print(f"接下来 {duration} 秒：继续画圈，每隔约 2 秒按一下 {name}。",
                      flush=True)
            try:
                deadline = begin + duration
                while time.monotonic() < deadline:
                    ready, _, _ = select.select(list(kinds), [], [],
                                                max(0, min(0.25, deadline - time.monotonic())))
                    for fd in ready:
                        try:
                            data = os.read(fd, EVENT.size * 128)
                        except BlockingIOError:
                            continue
                        if not data:
                            raise RuntimeError(f"{kinds[fd]} disconnected")
                        result["events"].extend(decode(data, kinds[fd], start, time.monotonic()))
            finally:
                phase["end"] = time.monotonic() - start


def analyze(result):
    events = sorted(result["events"], key=lambda event: event[1])
    frames, motion, releases, dropped = [], [], [], []
    moved = False
    for kind, stamp, event_type, code, value, received in events:
        if event_type == 0 and code == 3:
            dropped.append({"device": kind, "time": stamp})
        if kind != "touchpad":
            continue
        if event_type == 3 and code in (0, 1, 53, 54):
            moved = True
        if event_type == 1 and code == 330 and value == 0:
            releases.append(stamp)
        if event_type == 0 and code == 0:
            frames.append(stamp)
            if moved:
                motion.append(stamp)
            moved = False

    def cadence(times):
        gaps = [(right - left) * 1000 for left, right in zip(times, times[1:])]
        return {"frames": len(times),
                "median_gap_ms": round(statistics.median(gaps), 3) if gaps else None,
                "max_gap_ms": round(max(gaps), 3) if gaps else None}

    phases = []
    for phase in result["phases"]:
        lo, hi = phase["start"], phase["end"]
        phases.append({"name": phase["name"],
                       "motion": cadence([t for t in motion if lo <= t < hi]),
                       "touch_releases": sum(lo <= t < hi for t in releases),
                       "key_presses": sum(kind == "keyboard" and lo <= stamp < hi and
                                          event_type == 1 and value == 1
                                          for kind, stamp, event_type, _, value, _ in events)})

    edges = []
    for kind, stamp, event_type, code, value, received in events:
        if kind != "keyboard" or event_type != 1 or value not in (0, 1):
            continue
        at = bisect_left(motion, stamp)
        gaps = [(right - left) * 1000 for left, right in zip(motion, motion[1:])
                if right >= stamp and left <= stamp + 0.5]
        edges.append({"key": TEST_KEYS[code], "edge": "down" if value else "up",
                      "time": stamp,
                      "next_motion_ms": round((motion[at] - stamp) * 1000, 3)
                      if at < len(motion) else None,
                      "max_motion_gap_near_edge_ms": round(max(gaps), 3) if gaps else None,
                      "touch_releases_next_500ms": sum(stamp <= t <= stamp + 0.5
                                                       for t in releases)})
    before, after = result["status_before"], result["status_after"]
    resets = [key for key in COUNTERS if key in before and key in after
              and after[key] < before[key]]
    delta = {key: after[key] - before[key] for key in COUNTERS
             if key in before and key in after and key not in resets}
    return {"complete_without_evdev_overflow": not dropped and not result.get("error"),
            "syn_dropped": dropped, "counter_resets": resets, "counter_delta": delta,
            "raw_touch_frames": cadence(frames), "phases": phases, "key_edges": edges}


def print_summary(summary):
    print("\n采集结束，键盘已释放。以下是原始事件间隔，不是光标停顿的直接测量。")
    for phase in summary["phases"]:
        motion = phase["motion"]
        print(f"{phase['name']}: motion_frames={motion['frames']} "
              f"median_gap_ms={motion['median_gap_ms']} max_gap_ms={motion['max_gap_ms']} "
              f"touch_releases={phase['touch_releases']} key_presses={phase['key_presses']}")
    for edge in summary["key_edges"]:
        print(f"{edge['key']} {edge['edge']} @{edge['time']:.3f}s: "
              f"next_motion_ms={edge['next_motion_ms']} "
              f"max_gap_near_edge_ms={edge['max_motion_gap_near_edge_ms']} "
              f"touch_releases_next_500ms={edge['touch_releases_next_500ms']}")
    print("counter_delta=" + json.dumps(summary["counter_delta"], sort_keys=True))
    if not summary["complete_without_evdev_overflow"] or summary["counter_resets"]:
        print("采集不完整、事件溢出或计数器重置：请连同错误一起提供，不能直接据此归因。")
    print("没有运动事件或松手也会产生间隔；需要结合持续画圈时的现象分析。")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=Path(datetime.now().strftime("pogo-capture-%Y%m%d-%H%M%S.json")))
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("请用 sudo python3 运行，以读取并临时独占键盘事件。")
    devices = discover_inputs()
    result = {"version": 1, "devices": devices, "keyboard_grabbed": False,
              "event_columns": ["device", "time_s", "type", "code", "value", "read_time_s"],
              "events": [], "phases": [], "status_before": status_snapshot()}
    print("共约 25 秒；持续移动触摸板并按提示测试。键盘会临时独占，到时自动释放。",
          flush=True)
    with args.output.open("x", encoding="utf-8") as output:
        try:
            capture(devices, result)
        except (OSError, RuntimeError, KeyboardInterrupt) as error:
            result["error"] = str(error) or "Interrupted"
        result["status_after"] = status_snapshot()
        result["summary"] = analyze(result)
        json.dump(result, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print_summary(result["summary"])
    if result.get("error"):
        print("error=" + result["error"])
    for when in ("status_before", "status_after"):
        if "error" in result[when]:
            print(when + ": " + result[when]["error"])
    print(f"完整记录：{args.output.resolve()}")
    return 1 if result.get("error") else 0


if __name__ == "__main__":
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        raise SystemExit(str(error))
