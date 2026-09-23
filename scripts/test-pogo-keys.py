#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host regression of pogo key decoding, Fn lifetime and MCU touchpad restoration."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent.parent / "pogo/oneplus_pogo.c").read_text()


def function(name):
    start = source.rfind("\nstatic ", 0, source.index(f"{name}(")) + 1
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


start = source.index("struct oneplus_pogo_media_map {")
end = source.index("struct oneplus_pogo {")
mapping = source[start:end]
constants = "\n".join(line for line in source.splitlines() if line.startswith("#define POGO_"))
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>
typedef uint8_t u8;
typedef uint16_t u16;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BIT(n) (1u << (n))
struct input_dev { int unused; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool queued, disabled; };
struct mutex { bool held; };
struct oneplus_pogo;
struct device { struct oneplus_pogo *data; };
struct device_attribute { int unused; };
struct serdev_device { struct device dev; };
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define to_serdev_device(p) container_of(p, struct serdev_device, dev)
#define serdev_device_get_drvdata(p) ((p)->dev.data)
#define msecs_to_jiffies(ms) (ms)
#define system_wq NULL
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static int kstrtobool(const char *buf, bool *value)
{
    if (!strcmp(buf, "1")) *value = true;
    else if (!strcmp(buf, "0")) *value = false;
    else return -EINVAL;
    return 0;
}
struct oneplus_pogo {
    struct input_dev *keyboard;
    u8 old_keys[8];
    u16 old_keycodes[6];
    u8 old_media[4];
    u16 old_media_keycodes[2];
    bool fn_down;
    struct mutex lock;
    struct delayed_work touchpad_work;
    bool touchpad_target_enabled;
    u8 touchpad_restore_left;
    int touchpad_restore_error;
};
static int restore_queues, writes, write_error;
static bool hardware_enabled;
static bool mod_delayed_work(void *wq, struct delayed_work *work, int delay)
{
    assert(delay == 20);
    if (work->disabled) return false;
    work->queued = true;
    restore_queues++;
    return true;
}
static void disable_delayed_work_sync(struct delayed_work *work)
{ work->disabled = true; work->queued = false; }
static int oneplus_pogo_send_locked(struct oneplus_pogo *p, u8 command,
                                    const u8 *payload, size_t len)
{
    assert(p->lock.held); /* RX only queues; worker/sysfs own serialized TX. */
    assert(command == 0x3a && len == 3 && payload[0] == 0x11 && payload[1] == 1);
    assert(payload[2] <= 1);
    writes++;
    if (!write_error) hardware_enabled = !payload[2];
    return write_error;
}
static int keys[KEY_MAX + 1], events, scans, syncs;
static unsigned int last_scan;
static u16 get_unaligned_le16(const u8 *p) { return p[0] | (p[1] << 8); }
static void input_report_key(struct input_dev *dev, unsigned int code, int value)
{
    assert(code > KEY_RESERVED && code <= KEY_MAX);
    if (keys[code] == value) {
        /* Keyboard modifier bits are a snapshot, like input-core handling. */
        assert(code == KEY_LEFTCTRL || code == KEY_LEFTSHIFT || code == KEY_LEFTALT ||
               code == KEY_LEFTMETA || code == KEY_RIGHTCTRL || code == KEY_RIGHTSHIFT ||
               code == KEY_RIGHTALT || code == KEY_RIGHTMETA);
        return;
    }
    keys[code] = value;
    events++;
}
static void input_event(struct input_dev *dev, int type, int code, unsigned int value)
{ assert(type == EV_MSC && code == MSC_SCAN); scans++; last_scan = value; }
static void input_sync(struct input_dev *dev) { syncs++; }
'''
tests = r'''
static void report(struct oneplus_pogo *p, u16 a, u16 b)
{
    u8 wire[] = {a, a >> 8, b, b >> 8};
    oneplus_pogo_report_media(p, wire, sizeof(wire));
}
static void keyboard(struct oneplus_pogo *p, u8 modifiers, u8 a, u8 b)
{
    u8 wire[] = {modifiers, 0, a, b, 0, 0, 0, 0};
    mutex_lock(&p->lock);
    oneplus_pogo_report_keyboard(p, wire, sizeof(wire));
    mutex_unlock(&p->lock);
}
static void restore(struct oneplus_pogo *p)
{
    int iterations = 0;
    while (p->touchpad_work.queued) {
        assert(++iterations <= 3); /* Bounded even with a failed transport. */
        p->touchpad_work.queued = false;
        oneplus_pogo_touchpad_work(&p->touchpad_work.work);
    }
}
int main(void)
{
    struct oneplus_pogo p = {.touchpad_target_enabled = true};
    /* Physical order supplied by the user; screenshot uses keyboard page. */
    const u16 row[] = {0x70, 0x6f, 0, 0, 0, 0,
                       0xb6, 0xcd, 0xb5, 0xe2, 0xea, 0xe9};
    const u16 fkeys[] = {KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
                         KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12};
    for (unsigned int i = 0; i < ARRAY_SIZE(row); i++) {
        const u8 ordinary[] = {0, 0, 0x68, 0x6b, 0x46, 0x73};
        if (row[i]) report(&p, row[i], 0);
        else keyboard(&p, 0, ordinary[i], 0);
        assert(keys[fkeys[i]]);
        report(&p, 0, 0);
        keyboard(&p, 0, 0, 0);
        assert(!keys[fkeys[i]]);
    }
    report(&p, 0x224, 0);
    assert(keys[KEY_ESC] && !keys[KEY_BACK]);
    report(&p, 0, 0);
    assert(!keys[KEY_ESC]);

    keyboard(&p, 0, POGO_SEARCH_USAGE, 0);
    assert(keys[KEY_FN] && p.fn_down);
    keyboard(&p, 0, 0, 0);
    assert(!keys[KEY_FN] && !p.fn_down);

    keyboard(&p, 0, POGO_SEARCH_USAGE, 0);
    keyboard(&p, 0, 0x46, POGO_SEARCH_USAGE);
    assert(keys[KEY_SYSRQ] && !keys[KEY_F5]);
    keyboard(&p, 0, 0x46, 0); /* Fn released while screenshot is still down. */
    keyboard(&p, 0, 0, 0x46); /* Move held key to another keyboard slot. */
    assert(keys[KEY_SYSRQ] && !keys[KEY_F5]);
    keyboard(&p, 0, 0, 0);
    assert(!keys[KEY_SYSRQ]);
    keyboard(&p, 1, 0x46, 4); /* Ctrl + F5 + A. */
    assert(keys[KEY_LEFTCTRL] && keys[KEY_F5] && keys[KEY_A]);
    u8 late[] = {1, 0, 4, 0x46, POGO_SEARCH_USAGE, 0, 0, 0};
    oneplus_pogo_report_keyboard(&p, late, sizeof(late));
    assert(p.fn_down);
    keyboard(&p, 1, 4, 0x46);
    assert(keys[KEY_F5] && !keys[KEY_SYSRQ]);
    keyboard(&p, 0, 0, 0);
    report(&p, 0, 0);
    keyboard(&p, 0, 76, 41); /* Delete and native keyboard-page Escape. */
    assert(keys[KEY_DELETE] && keys[KEY_ESC]);
    keyboard(&p, 0, 0, 0);

    for (unsigned int i = 0; i < ARRAY_SIZE(oneplus_pogo_media_map); i++) {
        const struct oneplus_pogo_media_map *m = &oneplus_pogo_media_map[i];
        if (!m->fn_keycode) continue;
        keyboard(&p, 0, POGO_SEARCH_USAGE, 0);
        report(&p, m->usage, 0);
        assert(keys[KEY_FN] && p.fn_down && keys[m->fn_keycode]);
        int count = events;
        report(&p, 0, m->usage); /* Media snapshots must not reset keyboard Fn. */
        assert(events == count && p.fn_down);
        keyboard(&p, 0, 0, 0); /* Release Fn before media key. */
        report(&p, m->usage, 0);
        assert(!keys[KEY_FN] && keys[m->fn_keycode] && !keys[m->keycode]);
        report(&p, 0, 0);
        assert(!keys[m->fn_keycode]);
        /* A late keyboard Fn must not retype a held media key. */
        report(&p, m->usage, 0);
        keyboard(&p, 0, POGO_SEARCH_USAGE, 0);
        report(&p, 0, m->usage);
        assert(keys[m->keycode] && !keys[m->fn_keycode]);
        report(&p, 0, 0);
        assert(!keys[m->keycode] && p.fn_down);
        keyboard(&p, 0, 0, 0);
    }
    /* Real captured keyboard usages, including both touchpad states. */
    const u8 special[] = {0x68, 0x6b, 0x6c, 0x46, 0x73};
    const u16 base[] = {KEY_F3, KEY_F4, KEY_F4, KEY_F5, KEY_F6};
    const u16 alternate[] = {KEY_MICMUTE, KEY_TOUCHPAD_TOGGLE,
                             KEY_TOUCHPAD_TOGGLE, KEY_SYSRQ, KEY_SCREENLOCK};
    for (unsigned int i = 0; i < ARRAY_SIZE(special); i++) {
        keyboard(&p, 0, special[i], 0);
        assert(last_scan == (0x070000u | special[i]) && keys[base[i]]);
        keyboard(&p, 0, 0, 0);
        for (int order = 0; order < 2; order++) {
            keyboard(&p, 0, order ? special[i] : POGO_SEARCH_USAGE,
                     order ? POGO_SEARCH_USAGE : special[i]);
            assert(keys[KEY_FN] && keys[alternate[i]] && !keys[base[i]]);
            int count = events;
            keyboard(&p, 0, special[i], POGO_SEARCH_USAGE);
            assert(events == count);
            keyboard(&p, 0, special[i], 0);
            assert(!p.fn_down && keys[alternate[i]]);
            keyboard(&p, 0, 0, 0);
            assert(!keys[alternate[i]]);
        }
    }
    keyboard(&p, 0, 0x6b, 0);
    keyboard(&p, 0, 0x6c, 0); /* Changing MCU state releases/represses F4. */
    assert(keys[KEY_F4]);
    keyboard(&p, 0, 0, 0);
    keyboard(&p, 0, POGO_SEARCH_USAGE, 0);
    assert(last_scan == 0x70072 && keys[KEY_FN]);
    u8 short_keyboard[] = {0, 0, 0};
    oneplus_pogo_report_keyboard(&p, short_keyboard, sizeof(short_keyboard));
    assert(p.fn_down && keys[KEY_FN]);
    keyboard(&p, 0, 0, 0);
    report(&p, 0x224, 0x224); /* Malformed duplicate usage emits one press. */
    assert(keys[KEY_ESC]);
    report(&p, 0, 0);
    assert(!keys[KEY_ESC]);
    int count = events;
    report(&p, 0xbeef, 0);
    assert(last_scan == 0x0cbeef && events == count);
    u8 short_packet[] = { 0, 0, 0 };
    int old_syncs = syncs;
    oneplus_pogo_report_media(&p, short_packet, sizeof(short_packet));
    assert(syncs == old_syncs);
    report(&p, 0, 0);
    for (int i = 0; i <= KEY_MAX; i++) assert(!keys[i]);
    assert(scans > 0);
    restore(&p);
    /* The firmware toggles independently; plain F4 must restore both targets. */
    for (int target = 0; target < 2; target++) {
        for (u8 usage = 0x6b; usage <= 0x6c; usage++) {
            p.touchpad_target_enabled = target;
            hardware_enabled = !target; /* Autonomous MCU toggle on press. */
            int count = writes;
            keyboard(&p, 0, usage, 0);
            assert(keys[KEY_F4] && !keys[KEY_TOUCHPAD_TOGGLE]);
            assert(writes == count && p.touchpad_work.queued); /* No TX in RX. */
            int queues = restore_queues;
            keyboard(&p, 0, 0, usage); /* Reorder held key. */
            assert(restore_queues == queues);
            restore(&p);
            assert(writes == count + 3 && hardware_enabled == target);
            hardware_enabled = !target; /* Also cover toggle at key release. */
            keyboard(&p, 0, 0, 0);
            restore(&p);
            assert(hardware_enabled == target && !p.touchpad_restore_left);

            /* Desktop receives Fn+F4, while hardware target stays unchanged. */
            hardware_enabled = !target;
            keyboard(&p, 0, usage, POGO_SEARCH_USAGE);
            assert(keys[KEY_TOUCHPAD_TOGGLE] && !keys[KEY_F4]);
            keyboard(&p, 0, usage, 0); /* Fn released first. */
            restore(&p);
            assert(hardware_enabled == target && p.touchpad_target_enabled == target);
            keyboard(&p, 0, 0, 0);
            restore(&p);
        }
    }
    struct serdev_device dev = {.dev = {.data = &p}};
    keyboard(&p, 0, 0x6b, 0); /* A queued correction must respect a newer sysfs request. */
    assert(touchpad_enabled_store(&dev.dev, NULL, "0", 1) == 1);
    assert(!p.touchpad_target_enabled && !hardware_enabled);
    int count_writes = writes;
    restore(&p);
    assert(writes == count_writes);
    keyboard(&p, 0, 0, 0);
    restore(&p);
    assert(!hardware_enabled);
    write_error = -EIO;
    assert(touchpad_enabled_store(&dev.dev, NULL, "1", 1) == -EIO);
    assert(!p.touchpad_target_enabled); /* Failed request cannot replace the target. */
    assert(touchpad_enabled_store(&dev.dev, NULL, "bad", 3) == -EINVAL);
    keyboard(&p, 0, 0x6c, 0);
    count_writes = writes;
    restore(&p);
    assert(writes == count_writes + 3 && p.touchpad_restore_error == -EIO);
    keyboard(&p, 0, 0, 0);
    restore(&p);
    write_error = 0;
    assert(touchpad_enabled_store(&dev.dev, NULL, "1", 1) == 1);
    assert(p.touchpad_target_enabled && hardware_enabled);
    count_writes = writes;
    keyboard(&p, 0, 4, 0); /* Ordinary typing never sends touchpad commands. */
    keyboard(&p, 0, 0, 0);
    restore(&p);
    assert(writes == count_writes);
    keyboard(&p, 0, 0x6b, 0);
    oneplus_pogo_stop_touchpad_work(&p);
    keyboard(&p, 0, 0, 0); /* RX during removal must not requeue work. */
    restore(&p);
    assert(writes == count_writes && !p.touchpad_work.queued);
    for (int i = 0; i <= KEY_MAX; i++) assert(!keys[i]);
    puts("PASS: F4 hardware restoration on both edges/states, Fn desktop toggle, explicit disabled target, sysfs ordering, bounded TX failures, removal guard");
    puts("PASS: F1-F12 order, Esc/Delete, captured keyboard Fn across media reports, both touchpad states, screenshot/modifiers, release/slot ordering, invalid reports");
}
'''
with tempfile.TemporaryDirectory(prefix="pogo-keys-") as temp:
    path, binary = Path(temp) / "keys.c", Path(temp) / "keys"
    path.write_text(shim + constants + "\n" + mapping + "\n".join(
        function(name) for name in ("oneplus_pogo_set_touchpad_enabled_locked",
                                   "oneplus_pogo_queue_touchpad_restore", "oneplus_pogo_touchpad_work",
                                   "oneplus_pogo_stop_touchpad_work", "touchpad_enabled_store",
                                   "oneplus_pogo_key_present", "oneplus_pogo_keyboard_key",
                                   "oneplus_pogo_report_keyboard",
                                   "oneplus_pogo_media_key", "oneplus_pogo_media_present",
                                   "oneplus_pogo_report_media")) + tests)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                    "-fsanitize=undefined,bounds", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
