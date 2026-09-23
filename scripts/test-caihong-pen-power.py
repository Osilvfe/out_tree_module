#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production CPS power sequencing/ACK code with fake transport and GPIOs."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent.parent / 'charging/caihong_pen_power.c').read_text()


def function(name):
    start = source.rfind('\nstatic ', 0, source.index(name + '(')) + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define PEN_OWNER 32785
#define PEN_SET_HBOOST 0x10007
#define PEN_ACK_MS 2500
#define PMIC_GLINK_REQ_RESP 1
#define SERVREG_SERVICE_STATE_UP 0x1fffffff
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#define BIT(i) (1u << (i))
#define cpu_to_le32(x) (x)
#define msecs_to_jiffies(x) (x)
#define spin_lock_irqsave(l, f) do { (void)(l); (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(l); (void)(f); } while (0)
#define I2C_M_RD 1
#define dev_info(...) do {} while (0)
struct completion { bool done; };
struct gpio_desc { int value, id; };
struct i2c_client { void *adapter; };
struct i2c_msg { int addr, flags, len; u8 *buf; };
struct pmic_glink_hdr { u32 owner, type, opcode; };
struct pen_boost_request { struct pmic_glink_hdr hdr; u8 value, reserved[3]; };
struct pen_power {
    void *glink;
    struct i2c_client *i2c;
    struct gpio_desc *disable, *supply, *wake, *scan, *irq;
    int ack_lock;
    struct completion ack, lost;
    bool up, pending, poisoned, active, hardware_ready, attach_requested;
    int ack_error;
    u32 rejected, valid;
    u16 values[8];
    int result, cleanup;
    const char *phase;
};
static u32 get_unaligned_le32(const u8 *p)
{ return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put_unaligned_be16(u16 v, u8 *p) { p[0] = v >> 8; p[1] = v; }
static void complete(struct completion *c) { c->done = true; }
static void complete_all(struct completion *c) { c->done = true; }
static void reinit_completion(struct completion *c) { c->done = false; }
static bool completion_done(struct completion *c) { return c->done; }
static struct pen_power *current;
static int sends, reads, scenario, delays;
static bool supply_was_on;
static int pen_attach(struct pen_power *p) { assert(false); return -EINVAL; }
static void pen_reply(const void *, size_t, void *);
static void pen_transport(void *, int);
static int pmic_glink_send(void *client, void *data, size_t len)
{
    struct pen_boost_request *req = data;
    u32 reply[] = { PEN_OWNER, 1, PEN_SET_HBOOST, 0 };
    sends++;
    assert(len == 16 && req->hdr.owner == PEN_OWNER && req->hdr.type == 1);
    assert(req->hdr.opcode == PEN_SET_HBOOST);
    assert(!req->reserved[0] && !req->reserved[1] && !req->reserved[2]);
    assert(current->disable->value == 1 && current->supply->value == 0);
    assert(req->value == (sends == 1 ? 76 : 0));
    if (scenario == 1 || (scenario == 8 && sends == 2)) return 0; /* Lost ACK. */
    if (scenario == 2 && sends == 1) reply[3] = 1; /* NACK. */
    if (scenario == 3) { pen_transport(current, 0); return 0; }
    if (scenario == 4) { reply[2]++; pen_reply(reply, 16, current); return 0; }
    if (scenario == 9) return -EIO;
    pen_reply(reply, 16, current);
    return 0;
}
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long ms)
{
    if (c == &current->lost) {
        delays++;
        assert(ms == 10 || ms == 2500);
        if (scenario == 5 && ms == 2500) pen_transport(current, 0);
    }
    return c->done ? 1 : 0;
}
static void gpiod_set_value_cansleep(struct gpio_desc *g, int value)
{
    if (g == current->disable) assert(value == 1); /* Never allow charging. */
    if ((g == current->supply || g == current->wake) && value) {
        assert(current->disable->value == 1);
        assert(sends == 1 && !current->poisoned && current->up);
    }
    if (g == current->scan) assert(value == 0);
    if (g == current->supply && value) supply_was_on = true;
    g->value = value;
}
static int gpiod_get_value_cansleep(struct gpio_desc *g) { return g->value; }
static int i2c_transfer(void *adapter, struct i2c_msg *m, int n)
{
    static const u16 selectors[] = {0, 1, 2, 3, 4, 7, 8, 0x34, 0x35, 0x38, 0x39, 0x3a, 0x3e, 0x3f};
    assert(current->disable->value && current->supply->value && current->wake->value);
    assert(n == 2 && m[0].addr == 0x41 && m[1].addr == 0x41);
    assert(m[0].flags == 0 && m[1].flags == I2C_M_RD && m[0].len == 2 && m[1].len == 1);
    u16 reg = (u16)m[0].buf[0] << 8 | m[0].buf[1];
    assert(reg == selectors[reads++]);
    if (scenario == 6) return 1; /* Short transfer. */
    if (scenario == 10) return -ENXIO;
    m[1].buf[0] = reg == 0 ? 1 : reg == 1 ? (scenario == 7 ? 0 : 0x86) : reg;
    return 2;
}
'''
tests = r'''
int main(void)
{
    /* Transport-only stage must reject power even with no GPIO/I2C objects. */
    struct pen_power passive = { .up = true };
    current = &passive;
    assert(pen_run(&passive) == -EOPNOTSUPP);
    assert(!sends && !reads && !delays && !supply_was_on);
    for (scenario = 0; scenario <= 10; scenario++) {
        struct gpio_desc gpios[5] = {0};
        struct i2c_client client = {0};
        struct pen_power p = { .i2c = &client, .disable = &gpios[0], .supply = &gpios[1],
            .wake = &gpios[2], .scan = &gpios[3], .irq = &gpios[4],
            .up = true, .active = true, .hardware_ready = true };
        current = &p;
        sends = reads = delays = 0;
        supply_was_on = false;
        int result = pen_run(&p);
        assert(p.disable->value == 1 && !p.supply->value && !p.wake->value && !p.scan->value);
        switch (scenario) {
        case 0: assert(result == 0 && p.valid == 0xff && p.values[0] == 0x8601);
                assert(p.values[1] == 0x0302 && reads == 14 && sends == 2); break;
        case 1: case 4:
                assert(result == -ETIMEDOUT && p.poisoned && sends == 1);
                assert(!reads && !supply_was_on && p.cleanup == -EPIPE); break;
        case 2: assert(result == -EREMOTEIO && sends == 2 && !supply_was_on && !p.poisoned); break;
        case 3: case 5:
                assert(result == -ENOTCONN && p.poisoned && sends == 1 && !reads); break;
        case 6: assert(result == -EIO && reads == 1 && p.valid == 0 && sends == 2); break;
        case 7: assert(result == -ENODEV && reads == 2 && p.valid == 1 && sends == 2); break;
        case 8: assert(result == -ETIMEDOUT && p.poisoned && p.valid == 0xff && sends == 2); break;
        case 9: assert(result == -EIO && p.poisoned && sends == 1 && !supply_was_on); break;
        case 10: assert(result == -ENXIO && reads == 1 && !p.valid && sends == 2); break;
        }
        if (p.poisoned) {
            /* Late success cannot clear the ambiguity or allow another request. */
            u32 reply[] = { PEN_OWNER, 1, PEN_SET_HBOOST, 0 };
            pen_reply(reply, sizeof(reply), &p);
            int previous = sends;
            pen_transport(&p, SERVREG_SERVICE_STATE_UP);
            assert(p.poisoned && pen_set_boost(&p, 0) == -EPIPE && sends == previous);
        }
    }
    u8 bytes[17] = {0};
    u32 reply[] = { PEN_OWNER, 1, PEN_SET_HBOOST, 0 };
    memcpy(bytes + 1, reply, 16);
    assert(pen_reply_valid(bytes + 1, 16)); /* Unaligned transport payload. */
    for (size_t n = 0; n < 16; n++) assert(!pen_reply_valid(bytes + 1, n));
    assert(!pen_reply_valid(bytes, 17));
    reply[0]++;
    assert(!pen_reply_valid(reply, 16));
    reply[0]--; reply[1] = 2;
    assert(!pen_reply_valid(reply, 16));
    puts("PASS: real power/ACK callbacks, charge-inhibit invariant, ID/endian/read guards, NACK/short read, timeout/late reply, transport loss and cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='pen-power-') as temp:
    path, binary = Path(temp) / 'power.c', Path(temp) / 'power'
    names = ('pen_reply_valid', 'pen_reply', 'pen_transport', 'pen_set_boost',
             'pen_off', 'pen_delay', 'pen_read', 'pen_identify', 'pen_run')
    path.write_text(shim + ''.join(function(name) for name in names) + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-fsanitize=undefined,bounds', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
