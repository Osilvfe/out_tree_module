#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault-test the production bounded attachment/ASK path with fake hardware."""
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


exchange = source[source.index('struct pen_exchange {'):]
exchange = exchange[:exchange.index('\n};') + 4]
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
#define BIT(i) (1u << (i))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PEN_ATTACH_MS 15000
#define PEN_STARTUP_MS 2500
#define PEN_INT_ATTACH BIT(9)
#define PEN_INT_REMOVE BIT(10)
#define PEN_INT_ASK BIT(5)
#define PEN_INT_FAULT (BIT(6) | BIT(7) | BIT(8) | BIT(13))
#define dev_info(...) do {} while (0)
#define dev_err(...) do {} while (0)
#define msecs_to_jiffies(ms) (ms)
#define jiffies_to_msecs(j) (j)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool queued; unsigned long due; };
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
struct completion { bool done; };
struct gpio_desc { int value; };
struct i2c_client { void *adapter; };
struct i2c_msg { int addr, flags, len; u8 *buf; };
typedef int atomic_t;
static void atomic_set(atomic_t *p, int v) { *p = v; }
static int atomic_read(atomic_t *p) { return *p; }
static bool completion_done(struct completion *c) { return c->done; }
static void complete(struct completion *c) { c->done = true; }
static void reinit_completion(struct completion *c) { c->done = false; }
static void mutex_lock(int *p) { assert(!*p); *p = 1; }
static void mutex_unlock(int *p) { assert(*p); *p = 0; }
static void put_unaligned_be16(u16 v, u8 *p) { p[0] = v >> 8; p[1] = v; }
static void *memchr_inv(const void *ptr, int value, size_t n)
{ const u8 *p = ptr; for (size_t i = 0; i < n; i++) if (p[i] != value) return (void *)(p+i); return NULL; }
'''
hardware = r'''
struct pen_power {
    struct i2c_client *i2c;
    struct gpio_desc *disable, *supply, *wake, *scan;
    struct completion lost, event;
    struct delayed_work cutoff_work;
    atomic_t cutoff_fired, irq_count;
    struct pen_exchange exchange;
    int power_lock;
    const char *phase;
    u32 valid;
    u16 values[8];
    bool tx_requested, cycle_requested, startup_requested;
    u32 startup_ready_reads, startup_ready_ms;
};
static unsigned long jiffies;
static int system_unbound_wq;
static struct pen_power *current;
static unsigned int scenario, allow_count, event_index, writes;
static u8 registers[0x50];
static bool cutoff_checked;
static const u8 check_packet[11] = {0x48, 0xc1, 0x77, 0x55, 0x31};
static const u8 addr_packet[11] = {0x48, 0xb6, 0x25, 0x14, 0x23, 0x48, 0xb7, 0x52, 0x41, 0x30, 0};
static const u8 expected_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static void pen_cutoff(struct work_struct *w);
static int gpiod_get_value_cansleep(struct gpio_desc *g)
{
    if (scenario == 20 && current->exchange.enabled && g == current->scan) return 0;
    if (scenario == 49 && g == current->disable) return 0;
    return g->value;
}
static void gpiod_set_value_cansleep(struct gpio_desc *g, int v)
{
    if (g == current->supply && v)
        assert(!current->cutoff_fired && !current->lost.done);
    if (g == current->disable && !v) {
        assert(!current->cutoff_fired && current->cutoff_work.queued);
        assert(current->supply->value == 1 && current->scan->value == 1);
        assert(registers[0x28] == 0xf4 && registers[0x29] == 1);
        assert(registers[0x2a] == 0xe0 && registers[0x2b] == 0x2e);
        assert(registers[0x2c] == 0xa0 && registers[0x2d] == 0x0f);
        assert(registers[0x2f] == 0x90 && registers[0x30] == 1);
        allow_count++;
    }
    g->value = v;
}
static bool queue_delayed_work(int queue, struct delayed_work *w, unsigned long delay)
{
    if (scenario == 17) return false;
    assert(delay == (current->startup_requested ? PEN_STARTUP_MS : PEN_ATTACH_MS) && !w->queued);
    w->queued = true; w->due = jiffies + delay;
    if (scenario == 18) current->lost.done = true;
    return true;
}
static void cancel_delayed_work_sync(struct delayed_work *w) { w->queued = false; }
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long ms)
{
    if (c->done) { c->done = false; return 1; }
    jiffies += ms;
    if (scenario == 19 || scenario == 47) current->lost.done = true;
    struct delayed_work *w = &current->cutoff_work;
    if (w->queued && jiffies >= w->due) {
        w->queued = false;
        pen_cutoff(&w->work);
        assert(current->disable->value == 1 && !current->supply->value);
        cutoff_checked = true;
    }
    return 0;
}
static int pen_delay(struct pen_power *p, unsigned int ms)
{
    jiffies += ms;
    if (p->exchange.enabled && ms == 50 && !p->supply->value) {
        if (scenario == 30) p->lost.done = true;
        if (scenario == 31) pen_cutoff(&p->cutoff_work.work);
    }
    return p->lost.done ? -ENOTCONN : 0;
}
static void set_reg(u16 reg, u16 value) { registers[reg] = value; registers[reg+1] = value >> 8; }
static int i2c_transfer(void *adapter, struct i2c_msg *m, int n)
{
    assert(n == 1 && m->addr == 0x41 && !m->flags && m->len == 3);
    u16 reg = (u16)m->buf[0] << 8 | m->buf[1];
    assert(reg < ARRAY_SIZE(registers));
    writes++;
    if (current->startup_requested) assert(reg == 5 || reg == 6 || reg == 9 || reg == 10);
    if (scenario == 43 && reg == 5) return -EIO;
    if (reg == 0xb) {
        assert(current->tx_requested && current->exchange.enabled && m->buf[2] == 2);
        if (scenario == 25) return -EIO;
        registers[0xb] = 0; registers[4] = 1;
        return 1;
    }
    const u16 failed_regs[] = {0x28, 0x2c, 0x2a, 0x2f, 5};
    if (scenario >= 3 && scenario <= 7 && reg == failed_regs[scenario-3]) return -ENXIO;
    if (reg == 9 || reg == 10) {
        registers[reg-2] &= ~m->buf[2];
        if (reg == 10 && (current->exchange.enabled || current->startup_requested)) event_index++;
    } else registers[reg] = m->buf[2];
    return 1;
}
static int pen_read(struct pen_power *p, u16 reg, unsigned int len, u16 *value)
{
    if (p->lost.done) return -ENOTCONN;
    assert(reg + len <= ARRAY_SIZE(registers) && (len == 1 || len == 2));
    if (p->startup_requested) {
        if (reg == 0 && scenario == 53 && jiffies < 1500) return -ENXIO;
        if (reg == 0 && scenario == 54) return -EIO;
        if (reg == 0 && (scenario == 40 || (scenario == 39 && p->startup_ready_reads == 1))) return -ENXIO;
        if (reg == 0 && scenario == 41) { *value = 0; return 0; }
        if (reg == 2 && scenario == 42) { *value = 0x119; return 0; }
        if (reg == 0x38 && scenario == 51) { *value = 500; return 0; }
        if (reg == 7 && scenario == 50) {
            pen_cutoff(&p->cutoff_work.work);
            return -ENXIO;
        }
    }
    if (scenario == 34 && reg == 0x42) return -ENXIO;
    if (scenario == 35 && reg == 0x2c) return -EIO;
    if (p->exchange.cycled) {
        if (scenario == 29 && reg == 0x28) { *value = 600; return 0; }
        if (scenario == 32) return -ENXIO;
        if (scenario == 33 && reg == 0) { *value = 0; return 0; }
    }
    if (reg == 7 && (p->exchange.enabled || p->startup_requested)) {
        if (scenario == 10) return -EIO;
        if (scenario == 11) { *value = PEN_INT_REMOVE; return 0; }
        if (scenario == 12 || scenario == 48) { *value = BIT(6); return 0; }
        if (scenario == 16 || scenario == 19 || scenario == 36 || scenario == 44 || event_index >= 2) { *value = 0; return 0; }
        const u8 *packet = event_index ? addr_packet : check_packet;
        if (scenario == 21 || scenario == 38) packet = event_index ? check_packet : addr_packet;
        if (scenario == 22 || scenario == 46) { /* Address alone is insufficient. */
            if (event_index) { *value = 0; return 0; }
            packet = addr_packet;
        }
        memcpy(registers+0x40, packet, 11);
        if ((scenario == 2 || scenario == 45) && event_index == 1) registers[0x42] ^= 1;
        set_reg(7, PEN_INT_ASK | ((scenario == 23 || scenario == 52) && event_index == 1 ? PEN_INT_ATTACH : 0));
    }
    if (scenario == 8 && reg == 0x2f) { *value = 399; return 0; }
    if (scenario == 9 && reg == 7) { *value = 0x3d; return 0; }
    if (p->exchange.enabled && scenario == 13 && reg == 0x38) { *value = 500; return 0; }
    if (p->exchange.enabled && scenario == 14 && reg == 0x3a) { *value = 50; return 0; }
    if (p->exchange.enabled && scenario == 15 && reg == 0x34) { *value = 7000; return 0; }
    *value = registers[reg] | (len == 2 ? (u16)registers[reg+1] << 8 : 0);
    return 0;
}
'''
tests = r'''
int main(void)
{
    for (scenario = 0; scenario <= 54; scenario++) {
        struct gpio_desc gpios[4] = {{1},{1},{1},{0}};
        struct i2c_client client = {0};
        struct pen_power p = { .i2c=&client, .disable=&gpios[0], .supply=&gpios[1],
          .wake=&gpios[2], .scan=&gpios[3], .valid=0xff, .values={0x8601, 0x0118} };
        current=&p; jiffies=0; writes=allow_count=event_index=0; cutoff_checked=false;
        memset(registers, 0, sizeof(registers));
        set_reg(0, 0x8601); set_reg(2, 0x0118);
        set_reg(7, 0x3d); set_reg(0x34, 5800); set_reg(0x38, 125); registers[0x3a]=25;
        memcpy(registers+0x40, addr_packet, 11);
        if (scenario == 1) p.values[1]++;
        if (scenario >= 24 && scenario <= 26) p.tx_requested = true;
        if (scenario == 26) registers[0xb] = 0x20;
        if ((scenario >= 27 && scenario <= 33) || scenario == 35) {
            p.cycle_requested = true;
            set_reg(0x28, 500); set_reg(0x2c, 4000);
            set_reg(0x2a, 12000); set_reg(0x2f, 400);
        }
        if (scenario == 28) set_reg(0x28, 600);
        if (scenario >= 37) p.startup_requested = true;
        int ret=p.startup_requested ? pen_startup(&p) : pen_attach(&p);
        assert(!p.cutoff_work.queued);
        if (p.startup_requested) {
            assert(!allow_count && !p.exchange.enabled && !p.tx_requested && !p.cycle_requested);
            assert(p.disable->value == 1 && !p.supply->value && !p.wake->value && !p.scan->value);
            if (scenario <= 39 || scenario == 53) {
                assert(!ret && p.exchange.mac_valid && p.valid == 0xff);
                assert(p.exchange.checks == 1 && p.exchange.addresses == 1);
                assert(!memcmp(p.exchange.mac, expected_mac, 6));
            } else assert(ret < 0);
            if (scenario == 39) assert(p.startup_ready_reads == 2);
            if (scenario == 40) assert(jiffies == PEN_STARTUP_MS && ret == -ETIMEDOUT && !writes);
            if (scenario == 41 || scenario == 42) assert(!writes);
            if (scenario == 44 || scenario == 46 || scenario == 52)
                assert(ret == -ETIMEDOUT && !p.exchange.mac_valid && cutoff_checked);
            if (scenario == 45) assert(ret == -EBADMSG && p.exchange.invalid == 1);
            if (scenario == 47) assert(ret == -ENOTCONN);
            if (scenario == 48) assert(ret == -ECANCELED);
            if (scenario == 49) assert(ret == -EIO && !writes);
            if (scenario == 50) assert(ret == -ETIMEDOUT && !writes);
            if (scenario == 51) assert(ret == -ERANGE);
            if (scenario == 53) assert(p.startup_ready_ms == 1500);
            if (scenario == 54) assert(ret == -EIO && p.startup_ready_reads == 1 && !writes);
            unsigned int previous = writes;
            assert(pen_write(&p, 0x28, 2, 500) == -EPERM);
            assert(pen_write(&p, 0xb, 1, 2) == -EPERM && writes == previous);
            continue;
        }
        if (scenario == 0 || scenario == 21 || scenario == 24 || scenario == 27) {
            assert(!ret && p.exchange.mac_valid && allow_count == 1);
            assert(!memcmp(p.exchange.mac, expected_mac, 6));
            assert(p.exchange.checks == 1 && p.exchange.addresses == 1);
        } else assert(ret < 0 && !p.exchange.mac_valid);
        if ((scenario >= 1 && scenario <= 9 && scenario != 2) ||
            scenario == 17 || scenario == 18 || scenario == 26 || scenario == 28 ||
            scenario == 34 || scenario == 35)
            assert(!allow_count);
        if (scenario == 16 || scenario == 22 || scenario == 23)
            assert(ret == -ETIMEDOUT && cutoff_checked);
        if (scenario == 23) assert(p.exchange.attaches == 1 && !p.exchange.have_check);
        if (scenario == 2) assert(ret == -EBADMSG && p.exchange.invalid == 1);
        if (scenario == 19) assert(ret == -ENOTCONN);
        if (scenario == 20) assert(ret == -EIO);
        if (scenario == 24) assert(p.exchange.mode_after == 1 && !p.exchange.command_after);
        if (scenario == 27) assert(p.exchange.cycled && p.exchange.cycle_valid == 15);
        if (scenario == 29) assert(ret == -EIO && p.exchange.cycle_valid == 1);
        if (scenario == 30 || scenario == 31) assert(!p.exchange.cycled && !p.supply->value);
        if (scenario == 36) assert(ret == -ETIMEDOUT && !p.exchange.addresses && !p.exchange.checks);
        if (scenario != 1 && scenario != 34)
            assert(p.exchange.initial_raw_valid && !memcmp(p.exchange.initial_raw, addr_packet, 11));
        if (allow_count) assert(p.disable->value == 1 && !p.supply->value && !p.wake->value && !p.scan->value);
        unsigned int previous=writes;
        assert(pen_write(&p, 0xb, 1, 2) == -EPERM);
        assert(pen_write(&p, 0x1234, 2, 0) == -EPERM && writes == previous);
    }
    puts("PASS: 55 cases; attachment and early startup identity, bounded readiness, write restrictions, limits, loss/cutoff and cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='pen-attach-') as temp:
    path, binary = Path(temp)/'attach.c', Path(temp)/'attach'
    names = ('pen_off', 'pen_identify', 'pen_write', 'pen_write_checked', 'pen_packet', 'pen_sample',
             'pen_prepare_attach', 'pen_cycle', 'pen_cutoff', 'pen_receive', 'pen_attach', 'pen_startup')
    path.write_text(shim + exchange + hardware + ''.join(function(n) for n in names) + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=undefined,bounds',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
