#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read touch/pen, Bluetooth and CPS8601 status on Caihong.

Uses only the known I2C hub-3 address 0x41 and documented status registers.
Reads cached kernel diagnostic results when present. --probe-power explicitly
requests one bounded power/wake/ID test with charging inhibited. Does not scan
the bus, enable charging, clear IRQs, flash firmware, or pair Bluetooth.
No optional Python packages.
"""

import argparse
import ctypes
import errno
import fcntl
import os
from pathlib import Path
import re
import shutil
import subprocess


class I2CMessage(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("length", ctypes.c_uint16), ("buf", ctypes.POINTER(ctypes.c_uint8))]


class I2CTransfer(ctypes.Structure):
    _fields_ = [("msgs", ctypes.POINTER(I2CMessage)), ("nmsgs", ctypes.c_uint32)]


def read_reg(fd, register, length):
    # The vendor uses 16-bit big-endian register addresses and one-byte reads.
    # Multi-byte register values themselves are little-endian.
    data = bytearray()
    for address in range(register, register + length):
        selector = (ctypes.c_uint8 * 2)(address >> 8, address & 0xff)
        value = (ctypes.c_uint8 * 1)()
        messages = (I2CMessage * 2)(I2CMessage(0x41, 0, 2, selector),
                                  I2CMessage(0x41, 1, 1, value))
        transfer = I2CTransfer(messages, 2)
        count = fcntl.ioctl(fd, 0x0707, transfer)  # I2C_RDWR
        if count != 2:
            raise OSError(f"incomplete I2C transfer: {count}/2 messages")
        data.append(value[0])
    return int.from_bytes(data, "little")


def charger_gpio_status():
    """Read TLMM's existing debug snapshot without requesting/changing lines."""
    try:
        snapshot = Path("/sys/kernel/debug/gpio").read_text()
    except OSError as error:
        print(f"CPS8601 GPIO state: debugfs snapshot unavailable: {error}")
        return
    roles = {10: "supply switch", 12: "IRQ", 15: "wake/sleep",
             85: "scan", 111: "off-state"}
    tlmm = False
    found = set()
    for line in snapshot.splitlines():
        if re.match(r"gpiochip\d+:", line):
            tlmm = "f100000.pinctrl" in line
        if not tlmm:
            continue
        match = re.match(r"\s*gpio(\d+)\s*:", line)
        if match and int(match[1]) in roles:
            pin = int(match[1])
            found.add(pin)
            print(f"CPS8601 {roles[pin]}: {line.strip()}")
    if found != roles.keys():
        print("CPS8601 GPIO state: TLMM snapshot missing pins " +
              ", ".join(str(pin) for pin in sorted(roles.keys() - found)))
    print("CPS8601: GPIO output levels do not measure HBOOST voltage")


def charger_status(probe_power=False):
    provider = Path("/sys/bus/platform/devices/caihong-pen-power")
    if (provider / "status").exists():
        if probe_power:
            print("CPS8601: running bounded power/ID test with charging inhibited", flush=True)
            try:
                (provider / "probe_once").write_text("1\n")
            except OSError as error:
                print(f"CPS8601: power/ID test returned: {error}")
        try:
            print("CPS8601 cached power/ID diagnostic:\n" + (provider / "status").read_text().strip())
        except OSError as error:
            print(f"CPS8601: cached status unavailable: {error}")
        print("  valid bits 0..7: chip_id, firmware, mode, irq, VIN, IIN, temperature, EPT")
        print("  Values are latched during the test; supply is then off. No pen charging implemented.")
        return
    if probe_power:
        print("CPS8601: power diagnostic provider is not bound; check dmesg for caihong-pen-power")
        return
    adapters = []
    inventory = []
    # The i2c-adapter class was removed; adapters live on the I2C bus.
    # Restrict names to numeric adapters, not I2C clients such as i2c-ACPI:00.
    for adapter in sorted(Path("/sys/bus/i2c/devices").glob("i2c-*")):
        if not re.fullmatch(r"i2c-\d+", adapter.name):
            continue
        node = adapter / "of_node"
        if not node.exists():
            node = adapter / "device/of_node"
        inventory.append(f"{adapter.name}: {node.resolve() if node.exists() else 'no OF node'}")
        if node.exists() and node.resolve().name == "i2c@98c000":
            adapters.append(adapter)
    if len(adapters) != 1:
        print(f"CPS8601: expected one hub-3 adapter (i2c@98c000), found {len(adapters)}")
        print("  I2C adapters: " + ("; ".join(inventory) or "none registered"))
        return
    adapter = adapters[0]
    bus = adapter.name.removeprefix("i2c-")
    bound = Path(f"/sys/bus/i2c/devices/{bus}-0041/driver")
    if bound.exists():
        print(f"CPS8601: driver {bound.resolve().name} owns {bus}-0041; raw reads skipped")
        return
    device = f"/dev/{adapter.name}"
    try:
        fd = os.open(device, os.O_RDWR | os.O_CLOEXEC)
        try:
            fcntl.ioctl(fd, 0x0703, 0x41)  # I2C_SLAVE, never FORCE a bound client
            chip = read_reg(fd, 0x0000, 2)
            print(f"CPS8601: {device} address=0x41 chip_id=0x{chip:04x}")
            if chip != 0x8601:
                print("CPS8601: unexpected chip ID; remaining reads skipped")
                return
            registers = [("firmware", 0x0002, 2), ("mode", 0x0004, 1),
                         ("irq_flags", 0x0007, 2), ("vin_raw", 0x0034, 2),
                         ("iin_raw", 0x0038, 2), ("temperature_raw", 0x003a, 1),
                         ("ept_reason", 0x003e, 2)]
            for name, address, length in registers:
                value = read_reg(fd, address, length)
                print(f"  {name}={value} (0x{value:0{length * 2}x})")
            print("CPS8601: status read only; these values do not by themselves prove pen charging")
        finally:
            os.close(fd)
    except OSError as error:
        print(f"CPS8601: read failed: {error}")
        if error.errno == errno.ENXIO:
            print("  Adapter found, but address 0x41 did not acknowledge; check HBOOST, GPIO10 and GPIO15")
        print("  Supply, sleep state and bus access remain unconfirmed; no power controls changed")


def bluetooth_status(address):
    if not shutil.which("bluetoothctl"):
        print("Bluetooth: bluetoothctl is not installed")
        return
    commands = [["bluetoothctl", "list"], ["bluetoothctl", "show"]]
    if address:
        print("Bluetooth: pen info queries the BlueZ cache; 'not available' can mean it has not been discovered")
        commands.append(["bluetoothctl", "info", address])
    for command in commands:
        print("$ " + " ".join(command), flush=True)
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=8)
            print((result.stdout + result.stderr).strip() or f"no output (exit {result.returncode})")
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"Bluetooth: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="optional pen Bluetooth address; queries cached info only")
    parser.add_argument("--skip-charger", action="store_true", help="skip the I2C register reads")
    parser.add_argument("--probe-power", action="store_true",
                        help="explicitly run the kernel's single power/wake/ID test, then cut supply")
    args = parser.parse_args()
    if args.probe_power and args.skip_charger:
        parser.error("--probe-power cannot be combined with --skip-charger")
    if args.address and not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.address):
        parser.error("--address must be a six-byte Bluetooth address")
    for device in Path("/sys/bus/spi/devices").glob("spi*.*"):
        if not (device / "touch_stats").exists():
            continue
        for name in ("touch_stats", "pen_stats", "pen_scan"):
            path = device / name
            try:
                print(f"{device.name}/{name}: {path.read_text().strip()}")
            except OSError as error:
                print(f"{device.name}/{name}: {error}")
    for event in Path("/sys/class/input").glob("event*"):
        name_file = event / "device/name"
        try:
            name = name_file.read_text().strip()
        except OSError:
            continue
        if name.startswith("Novatek NT36532E"):
            print(f"input: /dev/input/{event.name} = {name}")
    bluetooth_status(args.address)
    if not args.skip_charger:
        charger_gpio_status()
        charger_status(args.probe_power)


if __name__ == "__main__":
    main()
