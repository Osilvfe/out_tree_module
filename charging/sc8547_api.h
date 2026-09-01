/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SC8547_BRINGUP_API_H
#define _SC8547_BRINGUP_API_H

#include <linux/types.h>

struct i2c_client;

enum sc8547_variant {
	SC8547_VARIANT_UNKNOWN,
	SC8547_VARIANT_SC8547,
	SC8547_VARIANT_SC8547A,
	SC8547_VARIANT_SC8547D,
};

struct sc8547_state {
	enum sc8547_variant variant;
	u8 device_id;
	bool initialized;
	bool stage4_authorized;
	bool enabled;
	bool switching;
	bool bypass;
	bool blocking_fault;
	int vbus_uv;
	int vbat_uv;
	int ibus_ua;
};

/*
 * Development-branch internal API shared by the physical SC8547 driver and
 * the Stage-5 virtual coordinator.  This is not a userspace or upstream ABI.
 * Callers must hold a reference to the i2c_client device while using it.
 */
int sc8547_get_state(struct i2c_client *client, struct sc8547_state *state);
int sc8547_set_manual_mode(struct i2c_client *client, bool bypass);
int sc8547_manual_preflight(struct i2c_client *client);
int sc8547_manual_enable(struct i2c_client *client);
int sc8547_manual_disable_client(struct i2c_client *client);

const char *sc8547_variant_to_name(enum sc8547_variant variant);

#endif /* _SC8547_BRINGUP_API_H */
