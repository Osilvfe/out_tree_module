#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pure safety and state tests for the automatic charge supervisor."""

import importlib.util
from pathlib import Path
import re


spec = importlib.util.spec_from_file_location(
    "charge", Path(__file__).with_name("caihong-pen-charge.py"))
charge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(charge)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    safe = "enabled=1 charge_disable=0\n"
    good = "removes=0 stop_packets=0 flags_seen=0x3d vin=5811 iin=120 temperature=25 ept=0x0"
    check(charge.status_safe(safe, good) == (True, "ok"), "normal status rejected")
    for key, value, reason in (
            ("iin", "251", "current-limit"),
            ("temperature", "46", "temperature-limit"),
            ("vin", "3999", "voltage-limit"),
            ("ept", "1", "ept"),
            ("removes", "1", "pen-removed-or-stopped"),
            ("stop_packets", "1", "pen-removed-or-stopped"),
            ("flags_seen", hex(1 << 6), "charger-fault-flag")):
        altered = re.sub(rf"\b{key}=\S+", f"{key}={value}", good)
        check(charge.status_safe(safe, altered) == (False, reason), reason)
    check(not charge.status_safe("enabled=1 charge_disable=1", good)[0],
          "inhibited charge accepted")
    check(charge.result_reason("ok", 0, False) == "completed",
          "successful request classified as failure")
    check(charge.result_reason("ok", -15, True) == "charge-request-timeout",
          "timed out request classification changed")
    check(charge.result_reason("ept", -15, False) == "ept",
          "safety reason was overwritten")
    check(charge.RETRY_DELAYS == (30, 60, 120, 300, 600), "backoff changed")
    print("PASS: automatic charge safety gates, fault limits, and backoff")


if __name__ == "__main__":
    main()
