#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production PM callbacks with simulated panel/bus order and faults.

This does not emulate kernel scheduling, IRQ delivery or physical panel power.
"""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent.parent / "touchscreen/nt36532e.c").read_text()


def function(name):
    start = source.rfind("\nstatic ", 0, source.index(f"{name}(")) + 1
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
struct device { void *data; };
struct spi_device { struct device dev; int irq; };
struct mutex { bool held; };
struct touchscreen_properties { int unused; };
struct drm_panel_follower { int unused; };
struct work_struct { bool pending; };
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define dev_info(dev, ...) ((void)(dev))
#define dev_warn(dev, ...) ((void)(dev))
#define dev_err(dev, ...) ((void)(dev))
#define IRQ_HANDLED 1
typedef int irqreturn_t;
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static void schedule_work(struct work_struct *w) { w->pending = true; }
static void cancel_work_sync(struct work_struct *w) { w->pending = false; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
'''
start = source.index("struct nt36532e {")
state = source[start:source.index("\n};", start) + 3]
constants = "\n".join(line for line in source.splitlines() if line.startswith("#define NVT_"))
stubs = r'''
static int irq_depth = 1, transfers, resets, releases;
static int detect_error, download_error, prepare_error, scan_error, sleep_error;
static void disable_irq_nosync(int irq) { assert(irq_depth == 0); irq_depth++; }
static void enable_irq(int irq) { assert(irq_depth == 1); irq_depth--; }
static void check_bus(struct nt36532e *ts)
{ assert(ts->lock.held && ts->panel_ready && !ts->suspended); transfers++; }
static void nvt_hw_reset(struct nt36532e *ts) { check_bus(ts); resets++; }
static int nvt_detect(struct nt36532e *ts) { check_bus(ts); return detect_error; }
static int nvt_download_fw(struct nt36532e *ts) { check_bus(ts); return download_error; }
static int nvt_prepare_events(struct nt36532e *ts) { check_bus(ts); return prepare_error; }
static int nvt_set_pen_scan(struct nt36532e *ts, unsigned int type)
{ check_bus(ts); return scan_error; }
static int nvt_set_page(struct nt36532e *ts, u32 page) { return sleep_error; }
static int nvt_spi_write(struct nt36532e *ts, const u8 *buf, size_t len)
{ assert(len == 2 && buf[1] == NVT_CMD_SLEEP); return sleep_error; }
static void input_mt_sync_frame(struct input_dev *d) { releases++; }
static void input_sync(struct input_dev *d) { }
static void nvt_pen_release(struct nt36532e *ts) { }
static void nvt_process_event(struct nt36532e *ts) { check_bus(ts); }
'''
tests = r'''
static void run_work(struct nt36532e *ts)
{
    if (ts->resume_work.pending) {
        ts->resume_work.pending = false;
        nvt_resume_work(&ts->resume_work);
    }
}
int main(void)
{
    struct spi_device spi = { .irq = 1 };
    struct nt36532e ts = { .spi = &spi, .pen_scan_type = -1 };
    spi.dev.data = &ts;
    nvt_panel_prepared(&ts.panel_follower);
    run_work(&ts);
    assert(ts.irq_enabled && ts.starts == 1 && irq_depth == 0);
    nvt_panel_prepared(&ts.panel_follower);
    run_work(&ts);
    assert(ts.starts == 1); /* Duplicate notification cannot unbalance IRQ. */

    /* Display blanking alone needs a firmware reload on the next prepare. */
    nvt_panel_unpreparing(&ts.panel_follower);
    int before = transfers;
    nvt_irq(1, &ts); /* Already queued IRQ must not touch sleeping hardware. */
    assert(transfers == before && !ts.irq_enabled && irq_depth == 1);
    nvt_panel_prepared(&ts.panel_follower);
    run_work(&ts);
    assert(ts.starts == 2 && ts.irq_enabled);

    /* Panel first, then SPI bus, on system resume. */
    nt36532e_suspend(&spi.dev);
    nvt_panel_unpreparing(&ts.panel_follower);
    before = transfers;
    nvt_panel_prepared(&ts.panel_follower);
    run_work(&ts);
    assert(transfers == before && !ts.irq_enabled);
    nt36532e_resume(&spi.dev);
    run_work(&ts);
    assert(ts.starts == 3 && ts.irq_enabled);

    /* SPI bus first, then panel; also suspend cancels queued startup. */
    nvt_panel_unpreparing(&ts.panel_follower);
    nvt_panel_prepared(&ts.panel_follower);
    nt36532e_suspend(&spi.dev);
    assert(!ts.resume_work.pending);
    nvt_panel_unpreparing(&ts.panel_follower);
    nt36532e_resume(&spi.dev);
    before = transfers;
    run_work(&ts);
    assert(transfers == before);
    nvt_panel_prepared(&ts.panel_follower);
    run_work(&ts);
    assert(ts.starts == 4 && ts.irq_enabled);

    /* Failed upload/info/detect stays disabled; next display cycle retries. */
    int *errors[] = { &download_error, &prepare_error, &detect_error };
    for (unsigned int i = 0; i < 3; i++) {
        nvt_panel_unpreparing(&ts.panel_follower);
        *errors[i] = -EIO;
        if (i == 2) ts.detected = false;
        nvt_panel_prepared(&ts.panel_follower);
        run_work(&ts);
        assert(!ts.irq_enabled && irq_depth == 1 && ts.start_error == -EIO);
        *errors[i] = 0;
        nvt_panel_unpreparing(&ts.panel_follower);
        nvt_panel_prepared(&ts.panel_follower);
        run_work(&ts);
        assert(ts.irq_enabled && irq_depth == 0 && !ts.start_error);
    }
    assert(ts.start_failures == 3);

    /* Optional pen command failure cannot prevent finger input resuming. */
    nvt_panel_unpreparing(&ts.panel_follower);
    ts.pen_scan_type = 1;
    scan_error = -ETIMEDOUT;
    nvt_panel_prepared(&ts.panel_follower);
    run_work(&ts);
    assert(ts.irq_enabled && ts.pen_scan_type == -1 && ts.pen_command_error == -ETIMEDOUT);
    sleep_error = -EIO;
    nvt_quiesce(&ts);
    assert(!ts.irq_enabled && ts.sleep_error == -EIO && ts.suspended);
    before = transfers;
    nvt_irq(1, &ts);
    nvt_quiesce(&ts);
    assert(transfers == before && irq_depth == 1 && releases > 0);
    puts("PASS: panel/bus resume ordering, blank/unblank, IRQ balance, queued work, startup/pen/sleep faults");
}
'''
with tempfile.TemporaryDirectory(prefix="nt36532e-pm-") as temp:
    path, binary = Path(temp) / "pm.c", Path(temp) / "pm"
    functions = ("nvt_irq", "nvt_start_events", "nvt_stop_events", "nvt_resume_work",
                 "nvt_panel_prepared", "nvt_panel_unpreparing", "nvt_quiesce",
                 "nt36532e_suspend", "nt36532e_resume")
    path.write_text(shim + constants + "\n" + state + stubs +
                    "\n".join(function(name) for name in functions) + tests)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=undefined,bounds",
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
