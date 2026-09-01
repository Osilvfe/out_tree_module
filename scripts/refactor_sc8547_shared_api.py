#!/usr/bin/env python3
from pathlib import Path

p = Path("charging/sc8547.c")
s = p.read_text()


def replace_once(old: str, new: str) -> None:
    global s
    count = s.count(old)
    if count != 1:
        raise SystemExit(f"expected exactly one match, got {count}: {old[:80]!r}")
    s = s.replace(old, new, 1)

replace_once(
    '#include <linux/sysfs.h>\n',
    '#include <linux/sysfs.h>\n\n#include "sc8547_api.h"\n',
)

anchor = '''static bool sc8547_has_blocking_fault(unsigned int reg06, unsigned int reg0e)\n{\n\treturn !!((reg06 & SC8547_BLOCKING_06) ||\n\t\t  (reg0e & SC8547_BLOCKING_0E));\n}\n'''
api_state = anchor + r'''

static enum sc8547_api_variant
sc8547_to_api_variant(enum sc8547_variant variant)
{
	switch (variant) {
	case SC8547_VARIANT_SC8547:
		return SC8547_API_VARIANT_SC8547;
	case SC8547_VARIANT_SC8547A:
		return SC8547_API_VARIANT_SC8547A;
	case SC8547_VARIANT_SC8547D:
		return SC8547_API_VARIANT_SC8547D;
	default:
		return SC8547_API_VARIANT_UNKNOWN;
	}
}

static struct sc8547_device *sc8547_from_client(struct i2c_client *client)
{
	if (!client || !client->dev.driver ||
	    strcmp(client->dev.driver->name, "sc8547"))
		return NULL;

	return i2c_get_clientdata(client);
}

int sc8547_get_state(struct i2c_client *client, struct sc8547_state *state)
{
	struct sc8547_device *sc = sc8547_from_client(client);
	unsigned int reg06, reg07, reg09, reg0e;
	int ret;

	if (!sc || !state)
		return -ENODEV;

	mutex_lock(&sc->lock);
	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &reg07);
	if (ret)
		goto out;
	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &reg09);
	if (ret)
		goto out;
	ret = sc8547_read_status(sc, &reg06, &reg0e);
	if (ret)
		goto out;

	state->variant = sc8547_to_api_variant(sc->variant);
	state->device_id = sc->device_id;
	state->initialized = sc->init_done;
	state->stage4_authorized = sc->allow_experimental_cp_enable &&
				   sc->window.complete &&
				   sc8547_variant_control_supported(sc->variant);
	state->enabled = !!(reg07 & SC8547_CHG_EN);
	state->switching = !!(reg06 & SC8547_CP_SWITCHING_STAT);
	state->bypass = !!(reg09 & SC8547_CHARGE_MODE);
	state->blocking_fault = sc8547_has_blocking_fault(reg06, reg0e);

	state->vbus_uv = sc8547_get_vbus_uv(sc);
	if (state->vbus_uv < 0) {
		ret = state->vbus_uv;
		goto out;
	}
	state->vbat_uv = sc8547_get_vbat_uv(sc);
	if (state->vbat_uv < 0) {
		ret = state->vbat_uv;
		goto out;
	}
	state->ibus_ua = sc8547_get_ibus_ua(sc);
	if (state->ibus_ua < 0) {
		ret = state->ibus_ua;
		goto out;
	}
	ret = 0;
out:
	mutex_unlock(&sc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8547_get_state);
'''
replace_once(anchor, api_state)

old_work = r'''static ssize_t work_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int reg;
	bool bypass;
	int ret;

	if (sysfs_streq(buf, "2:1"))
		bypass = false;
	else if (sysfs_streq(buf, "bypass"))
		bypass = true;
	else
		return -EINVAL;

	mutex_lock(&sc->lock);
	if (!sc->init_done || !sc->window.complete ||
	    !sc8547_variant_control_supported(sc->variant)) {
		ret = -EPERM;
		goto out;
	}

	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &reg);
	if (ret)
		goto out;
	if (reg & SC8547_CHG_EN) {
		ret = -EBUSY;
		goto out;
	}

	ret = sc8547_set_work_mode(sc, bypass);
	if (ret)
		goto out;
	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &reg);
	if (ret)
		goto out;
	if (!!(reg & SC8547_CHARGE_MODE) != bypass)
		ret = -EIO;
out:
	mutex_unlock(&sc->lock);
	if (ret)
		return ret;

	return count;
}
'''
new_work = r'''int sc8547_set_manual_mode(struct i2c_client *client, bool bypass)
{
	struct sc8547_device *sc = sc8547_from_client(client);
	unsigned int reg;
	int ret;

	if (!sc)
		return -ENODEV;

	mutex_lock(&sc->lock);
	if (!sc->allow_experimental_cp_enable || !sc->init_done ||
	    !sc->window.complete ||
	    !sc8547_variant_control_supported(sc->variant)) {
		ret = -EPERM;
		goto out;
	}

	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &reg);
	if (ret)
		goto out;
	if (reg & SC8547_CHG_EN) {
		ret = -EBUSY;
		goto out;
	}

	ret = sc8547_set_work_mode(sc, bypass);
	if (ret)
		goto out;
	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &reg);
	if (ret)
		goto out;
	if (!!(reg & SC8547_CHARGE_MODE) != bypass)
		ret = -EIO;
out:
	mutex_unlock(&sc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8547_set_manual_mode);

static ssize_t work_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	bool bypass;
	int ret;

	if (sysfs_streq(buf, "2:1"))
		bypass = false;
	else if (sysfs_streq(buf, "bypass"))
		bypass = true;
	else
		return -EINVAL;

	ret = sc8547_set_manual_mode(to_i2c_client(dev), bypass);
	if (ret)
		return ret;

	return count;
}
'''
replace_once(old_work, new_work)

anchor_preflight = r'''static int sc8547_enable_preflight(struct sc8547_device *sc)
{
	unsigned int reg06, reg07, reg0e;
	int ret;

	if (!sc->init_done || !sc->window.complete)
		return -EPERM;
	if (!sc8547_variant_control_supported(sc->variant))
		return -EOPNOTSUPP;

	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &reg07);
	if (ret)
		return ret;
	if (reg07 & SC8547_CHG_EN)
		return -EBUSY;

	ret = sc8547_read_status(sc, &reg06, &reg0e);
	if (ret)
		return ret;
	if (!(reg0e & SC8547_ADAPTER_INSERT_STAT) ||
	    !(reg0e & SC8547_VBAT_INSERT_STAT))
		return -ENODEV;
	if (reg06 & SC8547_CP_SWITCHING_STAT)
		return -EBUSY;
	if (sc8547_has_blocking_fault(reg06, reg0e))
		return -EIO;

	return sc8547_window_check(sc);
}
'''
preflight_api = anchor_preflight + r'''

int sc8547_manual_preflight(struct i2c_client *client)
{
	struct sc8547_device *sc = sc8547_from_client(client);
	int ret;

	if (!sc)
		return -ENODEV;

	mutex_lock(&sc->lock);
	if (!sc->allow_experimental_cp_enable)
		ret = -EPERM;
	else
		ret = sc8547_enable_preflight(sc);
	mutex_unlock(&sc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8547_manual_preflight);
'''
replace_once(anchor_preflight, preflight_api)

old_cp = r'''static ssize_t cp_enable_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&sc->lock);
	if (!enable) {
		ret = sc8547_manual_disable(sc);
		goto out;
	}

	ret = sc8547_enable_preflight(sc);
	if (ret)
		goto out;

	ret = sc8547_set_charge_enabled(sc, true);
	if (ret)
		goto out;

	msleep(SC8547_POST_ENABLE_MS);
	ret = sc8547_post_enable_check(sc);
	if (ret) {
		sc8547_set_charge_enabled(sc, false);
		goto out;
	}
out:
	mutex_unlock(&sc->lock);
	if (sc->psy)
		power_supply_changed(sc->psy);
	if (ret)
		return ret;

	return count;
}
'''
new_cp = r'''int sc8547_manual_enable(struct i2c_client *client)
{
	struct sc8547_device *sc = sc8547_from_client(client);
	int ret;

	if (!sc)
		return -ENODEV;

	mutex_lock(&sc->lock);
	if (!sc->allow_experimental_cp_enable) {
		ret = -EPERM;
		goto out;
	}

	ret = sc8547_enable_preflight(sc);
	if (ret)
		goto out;
	ret = sc8547_set_charge_enabled(sc, true);
	if (ret)
		goto out;

	msleep(SC8547_POST_ENABLE_MS);
	ret = sc8547_post_enable_check(sc);
	if (ret)
		sc8547_set_charge_enabled(sc, false);
out:
	mutex_unlock(&sc->lock);
	if (sc->psy)
		power_supply_changed(sc->psy);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8547_manual_enable);

int sc8547_manual_disable_client(struct i2c_client *client)
{
	struct sc8547_device *sc = sc8547_from_client(client);
	int ret;

	if (!sc)
		return -ENODEV;

	mutex_lock(&sc->lock);
	if (!sc->allow_experimental_cp_enable)
		ret = -EPERM;
	else
		ret = sc8547_manual_disable(sc);
	mutex_unlock(&sc->lock);
	if (sc->psy)
		power_supply_changed(sc->psy);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8547_manual_disable_client);

static ssize_t cp_enable_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	if (enable)
		ret = sc8547_manual_enable(to_i2c_client(dev));
	else
		ret = sc8547_manual_disable_client(to_i2c_client(dev));
	if (ret)
		return ret;

	return count;
}
'''
replace_once(old_cp, new_cp)

p.write_text(s)
print("SC8547 shared API refactor applied")
