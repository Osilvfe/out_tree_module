#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host packet tests of the actual driver functions, with SPI/input sinks.

This exercises the production decoder and FWINFO selection, not Linux IRQ
delivery, input-core slot tracking, GPIOs, or physical controller behavior.
Requires a host C compiler. No kernel headers or device access are needed.
"""

from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).resolve().parent.parent / "touchscreen/nt36532e.c").read_text()


def function(name):
    start = source.rfind("\nstatic ", 0, source.index(f"{name}(")) + 1
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


constants = "\n".join(line for line in source.splitlines() if line.startswith("#define NVT_"))
struct_start = source.index("struct nt36532e {")
state = source[struct_start:source.index("\n};", struct_start) + 3]
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
struct spi_device { int dev; };
struct gpio_desc { int unused; };
struct input_dev { int unused; };
struct touchscreen_properties { int unused; };
struct mutex { int unused; };
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err_probe(dev, err, ...) (err)
#define msleep(ms) ((void)0)
#define clamp_val(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define MT_TOOL_FINGER 0
#define ABS_MT_PRESSURE 0
'''
stubs = r'''
static u8 packet[NVT_POINT_DATA_LEN + 1], fwinfo[39];
static int bus_error, slot, reports, syncs, pen_calls;
static u32 reported_x, reported_y, reported_pressure;
static int nvt_set_page(struct nt36532e *ts, u32 page) { return bus_error; }
static int nvt_spi_read(struct nt36532e *ts, u8 *buf, size_t len)
{
    if (bus_error) return bus_error;
    if (buf[0] == NVT_EVENT_FWINFO) {
        assert(len == sizeof(fwinfo));
        memcpy(buf, fwinfo, len);
    } else {
        assert(len == sizeof(packet));
        memcpy(buf, packet, len);
    }
    return 0;
}
static void input_mt_slot(struct input_dev *dev, int id) { slot = id; }
static void input_mt_report_slot_state(struct input_dev *dev, int tool, bool active)
{ assert(active); reports++; }
static void touchscreen_report_pos(struct input_dev *dev,
    struct touchscreen_properties *prop, u32 x, u32 y, bool mt)
{ reported_x = x; reported_y = y; }
static void input_report_abs(struct input_dev *dev, int axis, u32 value)
{ reported_pressure = value; }
static void input_mt_sync_frame(struct input_dev *dev) { syncs++; }
static void input_sync(struct input_dev *dev) { }
static void nvt_report_pen(struct nt36532e *ts, const u8 *d) { pen_calls++; }
'''
tests = r'''
int main(void)
{
    struct nt36532e ts = { .max_x = 21200, .max_y = 30000 };

    /* 16-bit wire point: ID 1 enter, x=1234, y=5678, width=50. */
    memcpy(packet, (u8[]){0, 9, 0x04, 0xd2, 0x16, 0x2e, 50}, 7);
    packet[65] = 0xab;
    assert(nvt_point_checksum(packet));
    fwinfo[1] = 1; fwinfo[2] = 0xfe; fwinfo[13] = 0xf1;
    assert(nvt_prepare_events(&ts) == 0 && ts.high_res);
    nvt_process_event(&ts);
    assert(reports == 1 && slot == 0 && reported_x == 1234 && reported_y == 5678);
    assert(reported_pressure == 50 && ts.touch_contacts == 1 && syncs == 1);

    /* Bad touch checksum must not report coordinates or suppress pen dispatch. */
    packet[65] ^= 1; ts.pen_support = true;
    nvt_process_event(&ts);
    assert(reports == 1 && syncs == 1 && ts.checksum_errors == 1 && pen_calls == 1);
    ts.pen_support = false;

    /* A valid release frame must reach input_mt_sync_frame with no contacts. */
    memset(packet, 0, sizeof(packet));
    nvt_process_event(&ts);
    assert(syncs == 2 && reports == 1);

    /* A boot handshake is not a touch frame or a checksum failure. */
    memset(packet + 1, 0x77, 6);
    nvt_process_event(&ts);
    assert(syncs == 2 && ts.boot_events == 1 && ts.checksum_errors == 1);

    /* 12-bit layout must follow firmware even with high-resolution DT limits. */
    memset(packet, 0, sizeof(packet));
    fwinfo[13] = 0;
    assert(nvt_prepare_events(&ts) == 0 && !ts.high_res);
    memcpy(packet, (u8[]){0, 9, 0x4d, 0x23, 0x27, 10, 30}, 7);
    packet[65] = 0x38;
    assert(nvt_point_checksum(packet));
    nvt_process_event(&ts);
    assert(reports == 2 && reported_x == 1234 && reported_y == 567);
    assert(reported_pressure == 30 && syncs == 3);

    /* A coordinate equal to the DT size is outside its size-minus-one range. */
    ts.max_x = 1234;
    nvt_process_event(&ts);
    assert(reports == 2 && ts.out_of_range == 1 && syncs == 4);

    bus_error = -EIO;
    nvt_process_event(&ts);
    assert(syncs == 4 && ts.spi_errors == 1 && ts.last_error == -EIO);
    bus_error = 0;
    fwinfo[2] = 0;
    assert(nvt_prepare_events(&ts) == -EIO);
    puts("PASS: FWINFO layout, 16/12-bit points, checksums, pen dispatch, release frame, boot event, bounds and SPI failure");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nt36532e-events-") as temp:
    test_source = Path(temp) / "events.c"
    binary = Path(temp) / "events"
    test_source.write_text(shim + constants + "\n" + state + stubs + "\n".join(
        function(name) for name in ("nvt_point_checksum", "nvt_read_event",
                                   "nvt_prepare_events", "nvt_report_touch", "nvt_process_event")) + tests)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=undefined,bounds",
                    str(test_source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
