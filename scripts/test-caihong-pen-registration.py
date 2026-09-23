#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault-inject manual module setup; this cannot model kernel device-core locks."""
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define __init
#define __exit
#define GFP_KERNEL 0
#define PEN_NAME "caihong-pen-power"
#define PEN_OWNER 32785
#define PLATFORM_DEVID_NONE -1
#define DL_FLAG_AUTOPROBE_CONSUMER 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IS_ERR(p) (!(p))
#define PTR_ERR(p) (-ENOMEM)
#define pr_info(...) do {} while (0)
#define pr_err(...) do {} while (0)
#define dev_info(...) do {} while (0)
#define dev_err(...) do {} while (0)
struct device_node { int unused; };
struct device {
    struct device *parent;
    struct device_node *of_node;
    void *data;
    struct { int unused; } kobj;
};
struct platform_device { struct device dev; };
struct pen_power {
    struct device *dev;
    int result, cleanup, lock, ack_lock, ack, lost;
    void *glink;
    const char *phase;
    bool hardware_ready, suspended;
};
static struct device_node node;
static struct platform_device parent = { .dev.of_node = &node };
static struct platform_device child, *pen_device;
static struct pen_power data;
static int pen_driver, pen_gpios;
static int pen_pinmaps[1];
static void *pen_groups;
static unsigned int stage;
static int pen_probe_result;
static int failure, nodes, parents, devices, registered, lookups, locks, maps;
static int hw_calls, off_calls, allocs, glinks, wakes, groups;
static int pen_probe(struct platform_device *);
static void pen_remove(struct platform_device *);
static void pen_reply(void) {}
static void pen_transport(void) {}
static bool of_machine_is_compatible(const char *s) { return failure != 1; }
static struct device_node *of_find_node_by_path(const char *s)
{ if (failure == 2) return NULL; nodes++; return &node; }
static void of_node_put(struct device_node *n) { assert(nodes == 1); nodes--; }
static struct platform_device *of_find_device_by_node(struct device_node *n)
{ if (failure == 3) return NULL; parents++; return &parent; }
static void put_device(struct device *d) { assert(parents == 1); parents--; }
static bool device_trylock(struct device *d)
{ if (failure == 4) return false; assert(!locks); locks++; return true; }
static void device_unlock(struct device *d) { assert(locks == 1); locks--; }
static bool device_is_bound(struct device *d) { return failure != 5; }
static bool of_device_is_compatible(struct device_node *n, const char *s)
{ return failure != 6; }
static struct platform_device *platform_device_alloc(const char *s, int id)
{ if (failure == 7) return NULL; devices++; child = (struct platform_device){0}; return &child; }
static int platform_device_add(struct platform_device *p) { return failure == 8 ? -EIO : 0; }
static void platform_device_put(struct platform_device *p) { assert(devices == 1); devices--; }
static void *device_link_add(struct device *c, struct device *s, int flags)
{ return failure == 9 ? NULL : c; }
static void gpiod_add_lookup_table(void *p) { assert(!lookups); lookups++; }
static void gpiod_remove_lookup_table(void *p) { assert(lookups == 1); lookups--; }
static int pinctrl_register_mappings(const void *p, unsigned int n)
{ assert(stage == 2 && !maps); if (failure == 16) return -ENOMEM; maps++; return 0; }
static void pinctrl_unregister_mappings(const void *p) { assert(maps == 1); maps--; }
static void *devm_kzalloc(struct device *d, size_t s, int f)
{ if (failure == 11) return NULL; allocs++; data = (struct pen_power){0}; return &data; }
static void mutex_init(int *l) {}
static void spin_lock_init(int *l) {}
static void init_completion(int *c) {}
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static bool mutex_trylock(int *l) { if (*l) return false; *l = 1; return true; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void platform_set_drvdata(struct platform_device *p, void *v) { p->dev.data = v; }
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *devm_pmic_glink_client_alloc(struct device *d, int owner,
        void (*cb)(void), void (*notify)(void), void *v)
{ assert(d->parent == &parent.dev); return failure == 12 ? NULL : v; }
static void pmic_glink_client_register(void *c) { glinks++; }
static int pen_get_hardware(struct pen_power *p)
{ hw_calls++; assert(stage == 2 && lookups == 1 && maps == 1); if (failure == 13) return -EIO;
  p->hardware_ready = true; return 0; }
static void pen_off(struct pen_power *p) { assert(p->hardware_ready); off_calls++; }
static int device_init_wakeup(struct device *d, bool on)
{ if (on && failure == 14) return -ENOMEM; wakes += on ? 1 : -1; return 0; }
static int sysfs_create_groups(void *k, void *g)
{ if (failure == 15) return -ENOMEM; groups++; return 0; }
static void sysfs_remove_groups(void *k, void *g) { assert(groups == 1); groups--; }
/* devres release after a failed probe or remove. */
static void release_resources(void) { allocs = glinks = 0; }
static int platform_driver_register(void *d)
{ if (failure == 10) return -EIO; registered++; int ret = pen_probe(&child);
  if (ret) release_resources(); return 0; }
static void platform_driver_unregister(void *d)
{ assert(registered == 1); registered--;
  if (groups) { pen_remove(&child); release_resources(); } }
static void platform_device_unregister(struct platform_device *p)
{ if (groups) { pen_remove(p); release_resources(); } platform_device_put(p); }
'''

tests = r'''
static void reset(void)
{
    assert(!nodes && !parents && !devices && !registered && !lookups && !locks);
    assert(!allocs && !glinks && !wakes && !groups && !maps);
    pen_device = NULL; pen_probe_result = -ENODEV; hw_calls = off_calls = 0;
}
int main(void)
{
    for (stage = 0; stage <= 3; stage++) {
        reset(); failure = 0;
        int ret = pen_init();
        if (stage == 0 || stage == 3) { assert(ret == -EINVAL); continue; }
        assert(!ret && groups == 1 && glinks == 1 && registered == 1);
        assert(!nodes && !parents && !locks);
        assert(hw_calls == (stage == 2) && !off_calls);
        assert(!pen_suspend(&child.dev) && data.suspended);
        assert(!pen_resume(&child.dev) && !data.suspended);
        assert(off_calls == (stage == 2 ? 2 : 0));
        pen_exit();
        assert(off_calls == (stage == 2 ? 3 : 0));
    }
    for (stage = 1; stage <= 2; stage++) {
        for (failure = 1; failure <= 16; failure++) {
            if (stage == 1 && (failure == 13 || failure == 16)) continue;
            reset();
            assert(pen_init() < 0);
            assert(!groups && !registered);
            if (stage == 1) assert(!hw_calls && !off_calls);
        }
    }
    reset();
    puts("PASS: manual stage gating, transport-only lifecycle, synchronous failure propagation and registration cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='pen-registration-') as temp:
    path, binary = Path(temp) / 'registration.c', Path(temp) / 'registration'
    names = ('pen_setup', 'pen_probe', 'pen_stop', 'pen_remove',
             'pen_suspend', 'pen_resume', 'pen_init', 'pen_exit')
    path.write_text(shim + ''.join(function(name) for name in names) + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-misleading-indentation',
                    '-fsanitize=undefined,bounds', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
