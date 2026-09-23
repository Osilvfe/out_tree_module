#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host regression of the real pogo media-key decoder and Fn key lifetime."""
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
struct oneplus_pogo {
    struct input_dev *keyboard;
    u8 old_keys[8];
    u16 old_keycodes[6];
    u8 old_media[4];
    u16 old_media_keycodes[2];
    bool fn_down;
};
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
    oneplus_pogo_report_keyboard(p, wire, sizeof(wire));
}
int main(void)
{
    struct oneplus_pogo p = {0};
    /* Physical order supplied by the user; screenshot uses keyboard page. */
    const u16 row[] = {0x70, 0x6f, 0x391, 0x392, 0, 0x38e,
                       0xb6, 0xcd, 0xb5, 0xe2, 0xe9, 0xea};
    const u16 fkeys[] = {KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
                         KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12};
    for (unsigned int i = 0; i < ARRAY_SIZE(row); i++) {
        if (row[i]) report(&p, row[i], 0);
        else keyboard(&p, 0, 0x46, 0);
        assert(keys[fkeys[i]]);
        report(&p, 0, 0);
        keyboard(&p, 0, 0, 0);
        assert(!keys[fkeys[i]]);
    }
    report(&p, 0x224, 0);
    assert(keys[KEY_ESC] && !keys[KEY_BACK]);
    report(&p, 0, 0);
    assert(!keys[KEY_ESC]);

    report(&p, POGO_SEARCH_USAGE, 0);
    assert(keys[KEY_FN] && p.fn_down);
    report(&p, 0, 0);
    assert(!keys[KEY_FN] && !p.fn_down);

    report(&p, POGO_SEARCH_USAGE, 0);
    keyboard(&p, 0, 0x46, 0);
    assert(keys[KEY_SYSRQ] && !keys[KEY_F5]);
    report(&p, 0, 0); /* Fn released while screenshot is still down. */
    keyboard(&p, 0, 0, 0x46); /* Move held key to another keyboard slot. */
    assert(keys[KEY_SYSRQ] && !keys[KEY_F5]);
    keyboard(&p, 0, 0, 0);
    assert(!keys[KEY_SYSRQ]);
    keyboard(&p, 1, 0x46, 4); /* Ctrl + F5 + A. */
    assert(keys[KEY_LEFTCTRL] && keys[KEY_F5] && keys[KEY_A]);
    report(&p, POGO_SEARCH_USAGE, 0); /* Late Fn cannot change held F5. */
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
        /* Fn and row key arrive in either order in the same report. */
        for (int order = 0; order < 2; order++) {
            report(&p, order ? m->usage : POGO_SEARCH_USAGE,
                   order ? POGO_SEARCH_USAGE : m->usage);
            assert(keys[KEY_FN] && keys[m->fn_keycode]);
            int count = events;
            report(&p, POGO_SEARCH_USAGE, m->usage);
            assert(events == count); /* Slot reorder / duplicate frame. */
            report(&p, m->usage, 0); /* Release Fn first. */
            assert(!keys[KEY_FN] && keys[m->fn_keycode] && !keys[m->keycode]);
            report(&p, 0, 0);
            assert(!keys[m->fn_keycode]);
        }
        /* Pressing Fn after an existing F key must not retype that key. */
        report(&p, m->usage, 0);
        assert(keys[m->keycode]);
        report(&p, m->usage, POGO_SEARCH_USAGE);
        assert(keys[m->keycode] && !keys[m->fn_keycode]);
        report(&p, POGO_SEARCH_USAGE, 0);
        assert(!keys[m->keycode]);
        report(&p, 0, 0);
    }
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
    puts("PASS: F1-F12 order, Esc/Delete, Fn layers, screenshot/modifiers, release/slot ordering, invalid reports");
}
'''
with tempfile.TemporaryDirectory(prefix="pogo-keys-") as temp:
    path, binary = Path(temp) / "keys.c", Path(temp) / "keys"
    path.write_text(shim + constants + "\n" + mapping + "\n".join(
        function(name) for name in ("oneplus_pogo_key_present", "oneplus_pogo_keyboard_key",
                                   "oneplus_pogo_report_keyboard",
                                   "oneplus_pogo_media_key", "oneplus_pogo_media_present",
                                   "oneplus_pogo_report_media")) + tests)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                    "-fsanitize=undefined,bounds", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
