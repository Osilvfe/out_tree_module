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
#include <linux/input-event-codes.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
struct spi_device { int dev; };
struct gpio_desc { int unused; };
struct input_dev { int unused; };
struct touchscreen_properties { bool invert_x, invert_y, swap_x_y; };
struct mutex { int unused; };
struct drm_panel_follower { int unused; };
struct work_struct { int unused; };
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err_probe(dev, err, ...) (err)
#define msleep(ms) ((void)0)
#define clamp_val(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define clamp_t(t, x, lo, hi) clamp_val((t)(x), (t)(lo), (t)(hi))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define swap(a, b) do { int temp = (a); (a) = (b); (b) = temp; } while (0)
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MT_TOOL_FINGER 0
'''
stubs = r'''
static u8 packet[NVT_POINT_DATA_LEN + 1], fwinfo[39];
static int bus_error, slot, reports, syncs, command_writes, command_reads;
static u8 command_status, last_command;
static int abs_values[64], keys[768];
static struct touchscreen_properties *reported_prop;
static u32 reported_x, reported_y, reported_pressure;
static int nvt_set_page(struct nt36532e *ts, u32 page) { return bus_error; }
static int nvt_spi_read(struct nt36532e *ts, u8 *buf, size_t len)
{
    if (bus_error) return bus_error;
    if (buf[0] == NVT_EVENT_HOST_CMD) {
        assert(len == 3);
        command_reads++;
        buf[1] = command_status;
    } else if (buf[0] == NVT_EVENT_FWINFO) {
        assert(len == sizeof(fwinfo));
        memcpy(buf, fwinfo, len);
    } else {
        assert(len == sizeof(packet));
        memcpy(buf, packet, len);
    }
    return 0;
}
static int nvt_spi_write(struct nt36532e *ts, const u8 *buf, size_t len)
{
    if (bus_error) return bus_error;
    assert(len == 3 && buf[0] == NVT_EVENT_HOST_CMD && buf[1] == 0x7f);
    command_writes++;
    last_command = buf[2];
    return 0;
}
static void input_mt_slot(struct input_dev *dev, int id) { slot = id; }
static void input_mt_report_slot_state(struct input_dev *dev, int tool, bool active)
{ assert(active); reports++; }
static void touchscreen_report_pos(struct input_dev *dev,
    struct touchscreen_properties *prop, u32 x, u32 y, bool mt)
{ reported_x = x; reported_y = y; reported_prop = prop; }
static void input_report_abs(struct input_dev *dev, int axis, int value)
{ abs_values[axis] = value; if (axis == ABS_MT_PRESSURE) reported_pressure = value; }
static void input_report_key(struct input_dev *dev, int code, int value)
{ keys[code] = value; }
static void input_mt_sync_frame(struct input_dev *dev) { syncs++; }
static void input_sync(struct input_dev *dev) { }
'''
tests = r'''
int main(void)
{
    struct input_dev pen;
    struct nt36532e ts = { .max_x = 21200, .max_y = 30000,
        .pen = &pen, .pen_max_pressure = 16383, .pen_max_tilt = 60,
        .pen_prop = { .invert_x = true, .swap_x_y = true } };
    const u8 pen_point[] = {1, 4, 0xd2, 0x16, 0x2e, 8, 0, 0xf6, 20, 0, 1, 3, 80, 0x7f};

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
    memcpy(packet + 66, pen_point, sizeof(pen_point));
    nvt_process_event(&ts);
    assert(reports == 1 && syncs == 1 && ts.checksum_errors == 1 && ts.pen_reports == 1);
    assert(abs_values[ABS_PRESSURE] == 2048 && keys[BTN_TOUCH] && keys[BTN_TOOL_PEN]);
    assert(keys[BTN_STYLUS] == 1 && keys[BTN_STYLUS2] == 1);
    assert(abs_values[ABS_TILT_X] == 20 && abs_values[ABS_TILT_Y] == 10);
    assert(reported_prop == &ts.pen_prop && reported_x == 1234 && reported_y == 5678);
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

    ts.max_x = 21200;
    memcpy(packet + 66, pen_point, sizeof(pen_point));
    packet[79] ^= 1;
    nvt_report_pen(&ts, packet);
    assert(ts.pen_checksum_errors == 1 && abs_values[ABS_PRESSURE] == 2048);
    packet[79] ^= 1;
    packet[67] = 0xea; packet[68] = 0x60; packet[79] = 0x0b;
    nvt_report_pen(&ts, packet);
    assert(ts.pen_out_of_range == 1 && ts.pen_reports == 1);

    memcpy(packet + 66, pen_point, sizeof(pen_point));
    packet[71] = 0xff; packet[72] = 0xff; packet[79] = 0x89;
    nvt_report_pen(&ts, packet);
    assert(abs_values[ABS_PRESSURE] == 16383);
    memcpy(packet + 66, pen_point, sizeof(pen_point));
    packet[71] = 0; packet[79] = 0x87;
    nvt_report_pen(&ts, packet);
    assert(!keys[BTN_TOUCH] && keys[BTN_TOOL_PEN] && abs_values[ABS_DISTANCE] == 1);
    memset(packet + 66, 0, 14); packet[66] = 0xff; packet[79] = 1;
    nvt_report_pen(&ts, packet);
    assert(!keys[BTN_TOUCH] && !keys[BTN_TOOL_PEN] && !keys[BTN_STYLUS] && !keys[BTN_STYLUS2]);
    assert(!abs_values[ABS_PRESSURE] && !abs_values[ABS_TILT_X] && !abs_values[ABS_TILT_Y]);

    const u8 commands[] = {0x11, 0x10, 0x12, 0x13, 0x15, 0x18};
    for (unsigned int type = 0; type < sizeof(commands); type++) {
        assert(nvt_set_pen_scan(&ts, type) == 0);
        assert(last_command == commands[type]);
    }
    assert(command_writes == 6 && command_reads == 6);
    assert(nvt_set_pen_scan(&ts, 6) == -EINVAL && command_writes == 6);
    command_status = 0x7f;
    assert(nvt_set_pen_scan(&ts, 1) == -ETIMEDOUT);
    assert(command_writes == 7 && command_reads == 11);
    bus_error = -EIO;
    assert(nvt_set_pen_scan(&ts, 1) == -EIO && command_writes == 7);
    puts("PASS: touch packets, pen pressure/hover/exit/tilt/buttons/checksums/bounds, scan commands/ACK/timeout/errors");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nt36532e-events-") as temp:
    test_source = Path(temp) / "events.c"
    binary = Path(temp) / "events"
    test_source.write_text(shim + constants + "\n" + state + stubs + "\n".join(
        function(name) for name in ("nvt_point_checksum", "nvt_pen_checksum", "nvt_read_event",
                                   "nvt_prepare_events", "nvt_pen_release", "nvt_report_pen",
                                   "nvt_set_pen_scan", "nvt_report_touch", "nvt_process_event")) + tests)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=undefined,bounds",
                    str(test_source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
