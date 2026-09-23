#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of the passive CPS8601 diagnostic; never accesses real I2C."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "pen_status", Path(__file__).with_name("caihong-pen-status.py"))
status = importlib.util.module_from_spec(spec)
spec.loader.exec_module(status)
selectors = []
registers = {0: 0x01, 1: 0x86}
count = 2


def ioctl(fd, command, arg):
    assert fd == 91
    if command == 0x0703:
        assert arg == 0x41
        return 0
    assert command == 0x0707 and arg.nmsgs == 2
    write, read = arg.msgs[0], arg.msgs[1]
    assert write.addr == read.addr == 0x41
    assert write.flags == 0 and write.length == 2
    assert read.flags == 1 and read.length == 1
    address = (write.buf[0] << 8) | write.buf[1]
    selectors.append(address)
    read.buf[0] = registers.get(address, 0)
    return count


with patch.object(status.fcntl, "ioctl", side_effect=ioctl):
    registers.update({0x1234: 0xcd, 0x1235: 0xab})
    assert status.read_reg(91, 0x1234, 2) == 0xabcd
    assert selectors == [0x1234, 0x1235]
    count = 1
    try:
        status.read_reg(91, 0, 2)
    except OSError as error:
        assert "incomplete I2C transfer" in str(error)
    else:
        raise AssertionError("short transfers must fail")
    count = 2

    with tempfile.TemporaryDirectory(prefix="pen-status-") as temp:
        root = Path(temp)
        adapter = root / "sys/class/i2c-adapter/i2c-7"
        adapter.mkdir(parents=True)
        node = root / "firmware/devicetree/i2c@98c000"
        node.mkdir(parents=True)
        (adapter / "of_node").symlink_to(node)
        bound = root / "sys/bus/i2c/devices/7-0041/driver"
        bound.parent.mkdir(parents=True)
        with patch.object(status, "Path", side_effect=lambda p: root / p.lstrip("/")), \
                patch.object(status.os, "open", return_value=91) as opened, \
                patch.object(status.os, "close") as closed:
            selectors.clear()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status.charger_status()
            opened.assert_called_once_with("/dev/i2c-7", status.os.O_RDWR | status.os.O_CLOEXEC)
            closed.assert_called_once_with(91)
            assert selectors == [0, 1, 2, 3, 4, 7, 8, 0x34, 0x35, 0x38, 0x39, 0x3a, 0x3e, 0x3f]
            assert "chip_id=0x8601" in output.getvalue()
            registers[0] = 0
            selectors.clear()
            with contextlib.redirect_stdout(io.StringIO()):
                status.charger_status()
            assert selectors == [0, 1]  # Do not read past a mismatched chip.
            bound.symlink_to(node)
            opened.reset_mock()
            with contextlib.redirect_stdout(io.StringIO()):
                status.charger_status()
            opened.assert_not_called()  # Respect a bound kernel driver's ownership.
print("PASS: CPS selectors/endian, short-transfer rejection, exact register set, chip/driver guards")
