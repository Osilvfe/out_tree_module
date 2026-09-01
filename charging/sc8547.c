// SPDX-License-Identifier: GPL-2.0-only
/*
 * Southchip SC8547/SC8547A charge-pump bring-up driver.
 *
 * Standalone mainline-style port for OnePlus Pad Pro (caihong).
 *
 * Normal device nodes are telemetry-only apart from ADC enable.  Development
 * write controls are hidden behind explicit DT opt-ins.  Stage 4 provides only
 * manual single-pump lab control; there is no automatic fast-charge policy,
 * PD/PPS negotiation, dual-pump coordination or VOOC/UFCS protocol handling.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>

#include "sc8547_api.h"

#define SC8547_REG_BAT_OVP		0x00
#define SC8547_BAT_OVP_DIS		BIT(7)
#define SC8547_BAT_OVP_MASK		GENMASK(5, 0)
#define SC8547_BAT_OVP_BASE_MV		3500
#define SC8547_BAT_OVP_STEP_MV		25

#define SC8547_REG_BAT_OCP		0x01
#define SC8547_BAT_OCP_MASK		GENMASK(5, 0)
#define SC8547_BAT_OCP_BASE_MA		2000
#define SC8547_BAT_OCP_STEP_MA		100

#define SC8547_REG_AC_OVP		0x02
#define SC8547_AC_OVP_MASK		GENMASK(2, 0)
#define SC8547_AC_OVP_BASE_MV		11000
#define SC8547_AC_OVP_STEP_MV		1000

#define SC8547_REG_VBUS_OVP		0x04
#define SC8547_VBUS_OVP_DIS		BIT(7)
#define SC8547_VBUS_OVP_MASK		GENMASK(6, 0)
#define SC8547_VBUS_OVP_BASE_MV	6000
#define SC8547_VBUS_OVP_STEP_MV	50

#define SC8547_REG_IBUS_PROT		0x05
#define SC8547_IBUS_UCP_DIS		BIT(7)
#define SC8547_IBUS_OCP_DIS		BIT(6)
#define SC8547_IBUS_OCP_MASK		GENMASK(3, 0)
#define SC8547_IBUS_OCP_BASE_MA	1200
#define SC8547_IBUS_OCP_STEP_MA	300
#define SC8547_UCP_DEGLITCH_SC8547	BIT(5)
#define SC8547A_UCP_DEGLITCH_MASK	GENMASK(5, 4)

#define SC8547_REG_STATUS_06		0x06
#define SC8547_TSHUT_STAT		BIT(6)
#define SC8547_VBUS_ERRORLO_STAT	BIT(5)
#define SC8547_VBUS_ERRORHI_STAT	BIT(4)
#define SC8547_CP_SWITCHING_STAT	BIT(2)
#define SC8547_BLOCKING_06		(SC8547_TSHUT_STAT | \
					 SC8547_VBUS_ERRORLO_STAT | \
					 SC8547_VBUS_ERRORHI_STAT)

#define SC8547_REG_CHG_CTRL		0x07
#define SC8547_CHG_EN			BIT(7)
#define SC8547_REG_RESET		BIT(6)

#define SC8547_REG_SS_CTRL		0x08
#define SC8547_SS_TIMEOUT_MASK		GENMASK(7, 5)

#define SC8547_REG_MODE_CTRL		0x09
#define SC8547_CHARGE_MODE		BIT(7)
#define SC8547_WATCHDOG_MASK		GENMASK(2, 0)

#define SC8547_REG_PMID2OUT		0x0d
#define SC8547_PMID2OUT_UVP_MASK	GENMASK(7, 6)
#define SC8547_PMID2OUT_OVP_MASK	GENMASK(5, 4)

#define SC8547_REG_STATUS_0E		0x0e
#define SC8547_VOUT_OVP_STAT		BIT(7)
#define SC8547_VBAT_OVP_STAT		BIT(6)
#define SC8547_IBAT_OCP_STAT		BIT(5)
#define SC8547_VBUS_OVP_STAT		BIT(4)
#define SC8547_IBUS_OCP_STAT		BIT(3)
#define SC8547_IBUS_UCP_FALL_STAT	BIT(2)
#define SC8547_ADAPTER_INSERT_STAT	BIT(1)
#define SC8547_VBAT_INSERT_STAT		BIT(0)
#define SC8547_BLOCKING_0E		(SC8547_VOUT_OVP_STAT | \
					 SC8547_VBAT_OVP_STAT | \
					 SC8547_IBAT_OCP_STAT | \
					 SC8547_VBUS_OVP_STAT | \
					 SC8547_IBUS_OCP_STAT | \
					 SC8547_IBUS_UCP_FALL_STAT)

#define SC8547_REG_ADC_CTRL		0x11
#define SC8547_ADC_EN			BIT(7)

#define SC8547_REG_IBUS_ADC_H		0x13
#define SC8547_REG_VBUS_ADC_H		0x15
#define SC8547_REG_VAC_ADC_H		0x17
#define SC8547_REG_VOUT_ADC_H		0x19
#define SC8547_REG_VBAT_ADC_H		0x1b
#define SC8547_REG_TDIE_ADC_H		0x1f
#define SC8547_REG_DEVICE_ID		0x36

#define SC8547_ADC_12BIT_H_MASK		GENMASK(3, 0)
#define SC8547_TDIE_H_MASK		BIT(0)

#define SC8547A_DEVICE_ID		0x67
#define SC8547D_DEVICE_ID		0x49

#define SC8547_IBUS_UA_PER_LSB		1875
#define SC8547_VBUS_UV_PER_LSB		3750
#define SC8547_VAC_UV_PER_LSB		5000
#define SC8547_VOUT_UV_PER_LSB		1250
#define SC8547_VBAT_UV_PER_LSB		1250
#define SC8547_TDIE_MC_PER_LSB		500

#define SC8547_POST_ENABLE_MS		500

enum sc8547_variant {
	SC8547_VARIANT_UNKNOWN,
	SC8547_VARIANT_SC8547,
	SC8547_VARIANT_SC8547A,
	SC8547_VARIANT_SC8547D,
};

struct sc8547_chip_info {
	enum sc8547_variant variant;
	const char *name;
};

static const struct sc8547_chip_info sc8547_info = {
	.variant = SC8547_VARIANT_SC8547,
	.name = "sc8547",
};

static const struct sc8547_chip_info sc8547a_info = {
	.variant = SC8547_VARIANT_SC8547A,
	.name = "sc8547a",
};

struct sc8547_raw_profile {
	u8 reg00;
	u8 reg01;
	u8 reg02;
	u8 reg04;
	u8 reg05;
	u8 reg0d;
	bool has_reg01;
	bool has_reg0d;
	bool complete;
};

struct sc8547_enable_window {
	u32 vbus_min_uv;
	u32 vbus_max_uv;
	u32 vbat_min_uv;
	u32 vbat_max_uv;
	bool complete;
};

struct sc8547_device {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	const struct sc8547_chip_info *match_info;
	const char *role;
	enum sc8547_variant variant;
	struct mutex lock;
	struct sc8547_raw_profile profile;
	struct sc8547_enable_window window;
	bool allow_experimental_control;
	bool allow_experimental_cp_enable;
	bool init_done;
	u8 device_id;
};

static const struct regmap_config sc8547_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static const char *sc8547_variant_name(enum sc8547_variant variant)
{
	switch (variant) {
	case SC8547_VARIANT_SC8547:
		return "sc8547";
	case SC8547_VARIANT_SC8547A:
		return "sc8547a";
	case SC8547_VARIANT_SC8547D:
		return "sc8547d";
	default:
		return "unknown";
	}
}

static bool sc8547_variant_control_supported(enum sc8547_variant variant)
{
	return variant == SC8547_VARIANT_SC8547 ||
	       variant == SC8547_VARIANT_SC8547A;
}

static enum sc8547_variant
sc8547_detect_variant(struct sc8547_device *sc, u8 device_id)
{
	if (device_id == SC8547A_DEVICE_ID)
		return SC8547_VARIANT_SC8547A;
	if (device_id == SC8547D_DEVICE_ID)
		return SC8547_VARIANT_SC8547D;
	if (sc->match_info)
		return sc->match_info->variant;

	return SC8547_VARIANT_UNKNOWN;
}

/* Preserve variant-specific low bits when touching common controls. */
static int sc8547_set_charge_enabled(struct sc8547_device *sc, bool enable)
{
	return regmap_update_bits(sc->regmap, SC8547_REG_CHG_CTRL,
				 SC8547_CHG_EN, enable ? SC8547_CHG_EN : 0);
}

static int sc8547_set_work_mode(struct sc8547_device *sc, bool bypass)
{
	return regmap_update_bits(sc->regmap, SC8547_REG_MODE_CTRL,
				 SC8547_CHARGE_MODE,
				 bypass ? SC8547_CHARGE_MODE : 0);
}

static int sc8547_set_adc_enabled(struct sc8547_device *sc, bool enable)
{
	return regmap_update_bits(sc->regmap, SC8547_REG_ADC_CTRL,
				 SC8547_ADC_EN, enable ? SC8547_ADC_EN : 0);
}

static int sc8547_set_watchdog_code(struct sc8547_device *sc, unsigned int code)
{
	if (code > 5)
		return -EINVAL;

	return regmap_update_bits(sc->regmap, SC8547_REG_MODE_CTRL,
				 SC8547_WATCHDOG_MASK, code);
}

static int sc8547_watchdog_code_from_ms(unsigned int timeout_ms,
					unsigned int *code)
{
	switch (timeout_ms) {
	case 0:
		*code = 0;
		return 0;
	case 200:
		*code = 1;
		return 0;
	case 500:
		*code = 2;
		return 0;
	case 1000:
		*code = 3;
		return 0;
	case 5000:
		*code = 4;
		return 0;
	case 30000:
		*code = 5;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sc8547_read_adc(struct sc8547_device *sc, unsigned int reg,
			   unsigned int high_mask, int scale)
{
	unsigned int hi, lo;
	int ret;

	ret = regmap_read(sc->regmap, reg, &hi);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, reg + 1, &lo);
	if (ret)
		return ret;

	return (((hi & high_mask) << 8) | lo) * scale;
}

static int sc8547_get_vbus_uv(struct sc8547_device *sc)
{
	return sc8547_read_adc(sc, SC8547_REG_VBUS_ADC_H,
			       SC8547_ADC_12BIT_H_MASK, SC8547_VBUS_UV_PER_LSB);
}

static int sc8547_get_ibus_ua(struct sc8547_device *sc)
{
	return sc8547_read_adc(sc, SC8547_REG_IBUS_ADC_H,
			       SC8547_ADC_12BIT_H_MASK, SC8547_IBUS_UA_PER_LSB);
}

static int sc8547_get_vbat_uv(struct sc8547_device *sc)
{
	return sc8547_read_adc(sc, SC8547_REG_VBAT_ADC_H,
			       SC8547_ADC_12BIT_H_MASK, SC8547_VBAT_UV_PER_LSB);
}

static int sc8547_get_vout_uv(struct sc8547_device *sc)
{
	return sc8547_read_adc(sc, SC8547_REG_VOUT_ADC_H,
			       SC8547_ADC_12BIT_H_MASK, SC8547_VOUT_UV_PER_LSB);
}

static int sc8547_get_vac_uv(struct sc8547_device *sc)
{
	return sc8547_read_adc(sc, SC8547_REG_VAC_ADC_H,
			       SC8547_ADC_12BIT_H_MASK, SC8547_VAC_UV_PER_LSB);
}

static int sc8547_get_tdie_mc(struct sc8547_device *sc)
{
	return sc8547_read_adc(sc, SC8547_REG_TDIE_ADC_H,
			       SC8547_TDIE_H_MASK, SC8547_TDIE_MC_PER_LSB);
}

static int sc8547_read_status(struct sc8547_device *sc,
			       unsigned int *reg06, unsigned int *reg0e)
{
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_06, reg06);
	if (ret)
		return ret;

	return regmap_read(sc->regmap, SC8547_REG_STATUS_0E, reg0e);
}

static bool sc8547_has_blocking_fault(unsigned int reg06, unsigned int reg0e)
{
	return !!((reg06 & SC8547_BLOCKING_06) ||
		  (reg0e & SC8547_BLOCKING_0E));
}


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

static enum power_supply_property sc8547_psy_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static int sc8547_psy_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct sc8547_device *sc = power_supply_get_drvdata(psy);
	unsigned int reg;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = regmap_read(sc->regmap, SC8547_REG_STATUS_06, &reg);
		if (ret)
			return ret;
		val->intval = reg & SC8547_CP_SWITCHING_STAT ?
			POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = regmap_read(sc->regmap, SC8547_REG_STATUS_0E, &reg);
		if (ret)
			return ret;
		val->intval = !!(reg & SC8547_ADAPTER_INSERT_STAT);
		return 0;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &reg);
		if (ret)
			return ret;
		val->intval = !!(reg & SC8547_CHG_EN);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = sc8547_get_vbus_uv(sc);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = sc8547_get_ibus_ua(sc);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		ret = sc8547_get_tdie_mc(sc);
		if (ret < 0)
			return ret;
		val->intval = ret / 100;
		return 0;
	default:
		return -EINVAL;
	}
}

static ssize_t device_id_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%02x\n", sc->device_id);
}
static DEVICE_ATTR_RO(device_id);

static ssize_t variant_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", sc8547_variant_name(sc->variant));
}
static DEVICE_ATTR_RO(variant);

static ssize_t role_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", sc->role);
}
static DEVICE_ATTR_RO(role);

static ssize_t charge_enabled_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", !!(val & SC8547_CHG_EN));
}
static DEVICE_ATTR_RO(charge_enabled);

static ssize_t charge_mode_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n",
			  val & SC8547_CHARGE_MODE ? "bypass" : "2:1");
}
static DEVICE_ATTR_RO(charge_mode);

static ssize_t switching_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_06, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", !!(val & SC8547_CP_SWITCHING_STAT));
}
static DEVICE_ATTR_RO(switching);

static ssize_t adapter_present_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_0E, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", !!(val & SC8547_ADAPTER_INSERT_STAT));
}
static DEVICE_ATTR_RO(adapter_present);

static ssize_t battery_present_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_0E, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", !!(val & SC8547_VBAT_INSERT_STAT));
}
static DEVICE_ATTR_RO(battery_present);

#define SC8547_ADC_ATTR(_name, _fn) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct sc8547_device *sc = dev_get_drvdata(dev); \
	int ret = _fn(sc); \
	if (ret < 0) \
		return ret; \
	return sysfs_emit(buf, "%d\n", ret); \
} \
static DEVICE_ATTR_RO(_name)

SC8547_ADC_ATTR(vbus_uv, sc8547_get_vbus_uv);
SC8547_ADC_ATTR(ibus_ua, sc8547_get_ibus_ua);
SC8547_ADC_ATTR(vbat_uv, sc8547_get_vbat_uv);
SC8547_ADC_ATTR(vout_uv, sc8547_get_vout_uv);
SC8547_ADC_ATTR(vac_uv, sc8547_get_vac_uv);
SC8547_ADC_ATTR(tdie_mc, sc8547_get_tdie_mc);

static ssize_t status_regs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int reg06, reg0e;
	int ret;

	ret = sc8547_read_status(sc, &reg06, &reg0e);
	if (ret)
		return ret;

	return sysfs_emit(buf, "06=0x%02x 0e=0x%02x\n", reg06, reg0e);
}
static DEVICE_ATTR_RO(status_regs);

static ssize_t faults_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int reg06, reg0e;
	size_t len = 0;
	int ret;

	ret = sc8547_read_status(sc, &reg06, &reg0e);
	if (ret)
		return ret;

	if (reg06 & SC8547_TSHUT_STAT)
		len += sysfs_emit_at(buf, len, "thermal_shutdown ");
	if (reg06 & SC8547_VBUS_ERRORLO_STAT)
		len += sysfs_emit_at(buf, len, "vbus_low ");
	if (reg06 & SC8547_VBUS_ERRORHI_STAT)
		len += sysfs_emit_at(buf, len, "vbus_high ");
	if (reg0e & SC8547_VOUT_OVP_STAT)
		len += sysfs_emit_at(buf, len, "vout_ovp ");
	if (reg0e & SC8547_VBAT_OVP_STAT)
		len += sysfs_emit_at(buf, len, "vbat_ovp ");
	if (reg0e & SC8547_IBAT_OCP_STAT)
		len += sysfs_emit_at(buf, len, "ibat_ocp ");
	if (reg0e & SC8547_VBUS_OVP_STAT)
		len += sysfs_emit_at(buf, len, "vbus_ovp ");
	if (reg0e & SC8547_IBUS_OCP_STAT)
		len += sysfs_emit_at(buf, len, "ibus_ocp ");
	if (reg0e & SC8547_IBUS_UCP_FALL_STAT)
		len += sysfs_emit_at(buf, len, "ibus_ucp_fall ");

	if (!len)
		return sysfs_emit(buf, "none\n");

	buf[len - 1] = '\n';
	return len;
}
static DEVICE_ATTR_RO(faults);

static int sc8547_dump_range(struct sc8547_device *sc, char *buf, size_t *len,
			     unsigned int first, unsigned int last)
{
	unsigned int reg, val;
	int ret;

	for (reg = first; reg <= last; reg++) {
		ret = regmap_read(sc->regmap, reg, &val);
		if (ret)
			return ret;
		*len += sysfs_emit_at(buf, *len, "%02x:%02x\n", reg, val);
	}

	return 0;
}

static ssize_t register_dump_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	size_t len = 0;
	unsigned int val;
	int ret;

	ret = sc8547_dump_range(sc, buf, &len, 0x00, 0x23);
	if (ret)
		return ret;
	ret = sc8547_dump_range(sc, buf, &len, 0x2b, 0x33);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, 0x36, &val);
	if (ret)
		return ret;
	len += sysfs_emit_at(buf, len, "36:%02x\n", val);
	ret = sc8547_dump_range(sc, buf, &len, 0x3a, 0x3c);
	if (ret)
		return ret;

	return len;
}
static DEVICE_ATTR_RO(register_dump);

static int sc8547_ucp_deglitch_us(struct sc8547_device *sc, unsigned int reg05)
{
	unsigned int code;

	if (sc->variant == SC8547_VARIANT_SC8547)
		return reg05 & SC8547_UCP_DEGLITCH_SC8547 ? 5000 : 10;

	if (sc->variant != SC8547_VARIANT_SC8547A)
		return -EINVAL;

	code = FIELD_GET(SC8547A_UCP_DEGLITCH_MASK, reg05);
	switch (code) {
	case 0:
		return 10;
	case 1:
		return 5000;
	case 2:
		return 50000;
	case 3:
		return 100000;
	default:
		return -EINVAL;
	}
}

static int sc8547_watchdog_ms(unsigned int reg09)
{
	switch (FIELD_GET(SC8547_WATCHDOG_MASK, reg09)) {
	case 0:
		return 0;
	case 1:
		return 200;
	case 2:
		return 500;
	case 3:
		return 1000;
	case 4:
		return 5000;
	case 5:
		return 30000;
	default:
		return -EINVAL;
	}
}

static ssize_t protection_state_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int r00, r01, r02, r04, r05, r08, r09, r0d;
	unsigned int bat_ovp_code, bat_ocp_code, ac_ovp_code;
	unsigned int vbus_ovp_code, ibus_ocp_code;
	int deglitch_us, watchdog_ms;
	size_t len = 0;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_BAT_OVP, &r00);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_BAT_OCP, &r01);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_AC_OVP, &r02);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_VBUS_OVP, &r04);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_IBUS_PROT, &r05);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_SS_CTRL, &r08);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &r09);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_PMID2OUT, &r0d);
	if (ret)
		return ret;

	bat_ovp_code = FIELD_GET(SC8547_BAT_OVP_MASK, r00);
	bat_ocp_code = FIELD_GET(SC8547_BAT_OCP_MASK, r01);
	ac_ovp_code = FIELD_GET(SC8547_AC_OVP_MASK, r02);
	vbus_ovp_code = FIELD_GET(SC8547_VBUS_OVP_MASK, r04);
	ibus_ocp_code = FIELD_GET(SC8547_IBUS_OCP_MASK, r05);
	deglitch_us = sc8547_ucp_deglitch_us(sc, r05);
	watchdog_ms = sc8547_watchdog_ms(r09);

	len += sysfs_emit_at(buf, len,
		"raw 00=%02x 01=%02x 02=%02x 04=%02x 05=%02x 08=%02x 09=%02x 0d=%02x\n",
		r00, r01, r02, r04, r05, r08, r09, r0d);
	len += sysfs_emit_at(buf, len,
		"bat_ovp enabled=%u code=%u header_mv=%u\n",
		!(r00 & SC8547_BAT_OVP_DIS), bat_ovp_code,
		SC8547_BAT_OVP_BASE_MV + bat_ovp_code * SC8547_BAT_OVP_STEP_MV);
	len += sysfs_emit_at(buf, len,
		"bat_ocp code=%u header_ma=%u\n", bat_ocp_code,
		SC8547_BAT_OCP_BASE_MA + bat_ocp_code * SC8547_BAT_OCP_STEP_MA);
	len += sysfs_emit_at(buf, len,
		"ac_ovp code=%u header_formula_mv=%u\n", ac_ovp_code,
		SC8547_AC_OVP_BASE_MV + ac_ovp_code * SC8547_AC_OVP_STEP_MV);
	len += sysfs_emit_at(buf, len,
		"vbus_ovp enabled=%u code=%u header_mv=%u\n",
		!(r04 & SC8547_VBUS_OVP_DIS), vbus_ovp_code,
		SC8547_VBUS_OVP_BASE_MV + vbus_ovp_code * SC8547_VBUS_OVP_STEP_MV);
	len += sysfs_emit_at(buf, len,
		"ibus_ucp enabled=%u ibus_ocp enabled=%u code=%u header_ma=%u\n",
		!(r05 & SC8547_IBUS_UCP_DIS), !(r05 & SC8547_IBUS_OCP_DIS),
		ibus_ocp_code,
		SC8547_IBUS_OCP_BASE_MA + ibus_ocp_code * SC8547_IBUS_OCP_STEP_MA);
	if (deglitch_us >= 0)
		len += sysfs_emit_at(buf, len, "ibus_ucp_deglitch_us=%d\n",
				     deglitch_us);
	else
		len += sysfs_emit_at(buf, len,
				     "ibus_ucp_deglitch_us=unknown_for_variant\n");
	len += sysfs_emit_at(buf, len,
		"ss_timeout_code=%u pmid2out_uvp_code=%u pmid2out_ovp_code=%u\n",
		(unsigned int)FIELD_GET(SC8547_SS_TIMEOUT_MASK, r08),
		(unsigned int)FIELD_GET(SC8547_PMID2OUT_UVP_MASK, r0d),
		(unsigned int)FIELD_GET(SC8547_PMID2OUT_OVP_MASK, r0d));
	if (watchdog_ms >= 0)
		len += sysfs_emit_at(buf, len, "watchdog_ms=%d\n", watchdog_ms);
	else
		len += sysfs_emit_at(buf, len, "watchdog_ms=reserved_code_%u\n",
				     (unsigned int)FIELD_GET(SC8547_WATCHDOG_MASK, r09));
	len += sysfs_emit_at(buf, len,
		"note=header_* values are vendor-header decodes, not yet hardware-validated thresholds\n");

	return len;
}
static DEVICE_ATTR_RO(protection_state);

static struct attribute *sc8547_attrs[] = {
	&dev_attr_device_id.attr,
	&dev_attr_variant.attr,
	&dev_attr_role.attr,
	&dev_attr_charge_enabled.attr,
	&dev_attr_charge_mode.attr,
	&dev_attr_switching.attr,
	&dev_attr_adapter_present.attr,
	&dev_attr_battery_present.attr,
	&dev_attr_vbus_uv.attr,
	&dev_attr_ibus_ua.attr,
	&dev_attr_vbat_uv.attr,
	&dev_attr_vout_uv.attr,
	&dev_attr_vac_uv.attr,
	&dev_attr_tdie_mc.attr,
	&dev_attr_status_regs.attr,
	&dev_attr_faults.attr,
	&dev_attr_register_dump.attr,
	&dev_attr_protection_state.attr,
	NULL,
};

static const struct attribute_group sc8547_attr_group = {
	.name = "sc8547",
	.attrs = sc8547_attrs,
};

static int sc8547_read_u8_property(struct device *dev, const char *name, u8 *val)
{
	u32 tmp;
	int ret;

	ret = device_property_read_u32(dev, name, &tmp);
	if (ret)
		return ret;
	if (tmp > 0xff)
		return -ERANGE;

	*val = tmp;
	return 0;
}

static int sc8547_read_optional_u8_property(struct device *dev,
					    const char *name, u8 *val,
					    bool *present)
{
	u32 tmp;
	int ret;

	if (!device_property_present(dev, name)) {
		*present = false;
		return 0;
	}

	ret = device_property_read_u32(dev, name, &tmp);
	if (ret)
		return ret;
	if (tmp > 0xff)
		return -ERANGE;

	*present = true;
	*val = tmp;
	return 0;
}

static void sc8547_parse_experimental_profile(struct sc8547_device *sc)
{
	struct device *dev = sc->dev;
	bool complete = true;

	sc->allow_experimental_control =
		device_property_read_bool(dev, "southchip,allow-experimental-control");
	if (!sc->allow_experimental_control)
		return;

	if (sc8547_read_u8_property(dev, "southchip,experimental-reg00",
				    &sc->profile.reg00))
		complete = false;
	if (sc8547_read_u8_property(dev, "southchip,experimental-reg02",
				    &sc->profile.reg02))
		complete = false;
	if (sc8547_read_u8_property(dev, "southchip,experimental-reg04",
				    &sc->profile.reg04))
		complete = false;
	if (sc8547_read_u8_property(dev, "southchip,experimental-reg05",
				    &sc->profile.reg05))
		complete = false;
	if (sc8547_read_optional_u8_property(dev, "southchip,experimental-reg01",
					     &sc->profile.reg01,
					     &sc->profile.has_reg01))
		complete = false;
	if (sc8547_read_optional_u8_property(dev, "southchip,experimental-reg0d",
					     &sc->profile.reg0d,
					     &sc->profile.has_reg0d))
		complete = false;

	sc->profile.complete = complete;
	if (!complete)
		dev_warn(dev,
			 "experimental controls enabled but raw protection profile is incomplete/invalid\n");
}

static void sc8547_parse_enable_window(struct sc8547_device *sc)
{
	struct device *dev = sc->dev;
	struct sc8547_enable_window *w = &sc->window;
	bool requested;
	int ret;

	requested = device_property_read_bool(dev,
				       "southchip,allow-experimental-cp-enable");
	if (!requested)
		return;

	if (!sc->allow_experimental_control) {
		dev_warn(dev,
			 "CP-enable opt-in ignored without southchip,allow-experimental-control\n");
		return;
	}

	sc->allow_experimental_cp_enable = true;

	ret = device_property_read_u32(dev, "southchip,experimental-vbus-min-uv",
				       &w->vbus_min_uv);
	ret |= device_property_read_u32(dev, "southchip,experimental-vbus-max-uv",
					&w->vbus_max_uv);
	ret |= device_property_read_u32(dev, "southchip,experimental-vbat-min-uv",
					&w->vbat_min_uv);
	ret |= device_property_read_u32(dev, "southchip,experimental-vbat-max-uv",
					&w->vbat_max_uv);
	if (ret || !w->vbus_min_uv || !w->vbus_max_uv ||
	    !w->vbat_min_uv || !w->vbat_max_uv ||
	    w->vbus_min_uv >= w->vbus_max_uv ||
	    w->vbat_min_uv >= w->vbat_max_uv) {
		dev_warn(dev,
			 "experimental CP-enable requested with incomplete/invalid voltage window\n");
		return;
	}

	w->complete = true;
}

static int sc8547_fail_closed(struct sc8547_device *sc)
{
	int first = 0;
	int ret;

	ret = sc8547_set_charge_enabled(sc, false);
	if (ret)
		first = ret;
	ret = sc8547_set_watchdog_code(sc, 0);
	if (ret && !first)
		first = ret;

	return first;
}

static int sc8547_verify_reg(struct sc8547_device *sc, unsigned int reg,
			     u8 expected)
{
	unsigned int val;
	int ret;

	ret = regmap_read(sc->regmap, reg, &val);
	if (ret)
		return ret;

	return val == expected ? 0 : -EIO;
}

static int sc8547_profile_readback(struct sc8547_device *sc)
{
	const struct sc8547_raw_profile *p = &sc->profile;
	int ret;

	ret = sc8547_verify_reg(sc, SC8547_REG_BAT_OVP, p->reg00);
	if (ret)
		return ret;
	ret = sc8547_verify_reg(sc, SC8547_REG_AC_OVP, p->reg02);
	if (ret)
		return ret;
	ret = sc8547_verify_reg(sc, SC8547_REG_VBUS_OVP, p->reg04);
	if (ret)
		return ret;
	ret = sc8547_verify_reg(sc, SC8547_REG_IBUS_PROT, p->reg05);
	if (ret)
		return ret;
	if (p->has_reg01) {
		ret = sc8547_verify_reg(sc, SC8547_REG_BAT_OCP, p->reg01);
		if (ret)
			return ret;
	}
	if (p->has_reg0d) {
		ret = sc8547_verify_reg(sc, SC8547_REG_PMID2OUT, p->reg0d);
		if (ret)
			return ret;
	}

	return 0;
}

static int sc8547_apply_experimental_init(struct sc8547_device *sc)
{
	const struct sc8547_raw_profile *p = &sc->profile;
	int ret;

	if (!sc8547_variant_control_supported(sc->variant))
		return -EOPNOTSUPP;
	if (!p->complete)
		return -EINVAL;

	sc->init_done = false;
	ret = sc8547_fail_closed(sc);
	if (ret)
		return ret;

	ret = regmap_update_bits(sc->regmap, SC8547_REG_CHG_CTRL,
				 SC8547_REG_RESET, SC8547_REG_RESET);
	if (ret)
		goto fail;
	usleep_range(1000, 2000);

	ret = sc8547_fail_closed(sc);
	if (ret)
		goto fail;

	ret = regmap_write(sc->regmap, SC8547_REG_BAT_OVP, p->reg00);
	if (ret)
		goto fail;
	if (p->has_reg01) {
		ret = regmap_write(sc->regmap, SC8547_REG_BAT_OCP, p->reg01);
		if (ret)
			goto fail;
	}
	ret = regmap_write(sc->regmap, SC8547_REG_AC_OVP, p->reg02);
	if (ret)
		goto fail;
	ret = regmap_write(sc->regmap, SC8547_REG_VBUS_OVP, p->reg04);
	if (ret)
		goto fail;
	ret = regmap_write(sc->regmap, SC8547_REG_IBUS_PROT, p->reg05);
	if (ret)
		goto fail;
	if (p->has_reg0d) {
		ret = regmap_write(sc->regmap, SC8547_REG_PMID2OUT, p->reg0d);
		if (ret)
			goto fail;
	}

	ret = sc8547_fail_closed(sc);
	if (ret)
		goto fail;
	ret = sc8547_set_adc_enabled(sc, true);
	if (ret)
		goto fail;
	ret = sc8547_profile_readback(sc);
	if (ret)
		goto fail;

	sc->init_done = true;
	return 0;

fail:
	sc8547_fail_closed(sc);
	sc->init_done = false;
	return ret;
}

static ssize_t profile_raw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	const struct sc8547_raw_profile *p = &sc->profile;
	size_t len = 0;

	if (!p->complete)
		return sysfs_emit(buf, "incomplete\n");

	len += sysfs_emit_at(buf, len,
			     "required 00=%02x 02=%02x 04=%02x 05=%02x\n",
			     p->reg00, p->reg02, p->reg04, p->reg05);
	if (p->has_reg01)
		len += sysfs_emit_at(buf, len, "optional 01=%02x ", p->reg01);
	else
		len += sysfs_emit_at(buf, len, "optional 01=absent ");
	if (p->has_reg0d)
		len += sysfs_emit_at(buf, len, "0d=%02x\n", p->reg0d);
	else
		len += sysfs_emit_at(buf, len, "0d=absent\n");

	return len;
}
static DEVICE_ATTR_RO(profile_raw);

static ssize_t init_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n",
			  sc->init_done ? "initialized" : "not_initialized");
}
static DEVICE_ATTR_RO(init_state);

static ssize_t apply_init_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	mutex_lock(&sc->lock);
	ret = sc8547_apply_experimental_init(sc);
	mutex_unlock(&sc->lock);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(apply_init);

static ssize_t watchdog_ms_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int reg;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &reg);
	if (ret)
		return ret;

	ret = sc8547_watchdog_ms(reg);
	if (ret < 0)
		return sysfs_emit(buf, "reserved_code_%u\n",
				  (unsigned int)FIELD_GET(SC8547_WATCHDOG_MASK, reg));

	return sysfs_emit(buf, "%d\n", ret);
}

static ssize_t watchdog_ms_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int timeout_ms, code;
	int ret;

	ret = kstrtouint(buf, 0, &timeout_ms);
	if (ret)
		return ret;
	ret = sc8547_watchdog_code_from_ms(timeout_ms, &code);
	if (ret)
		return ret;

	mutex_lock(&sc->lock);
	if (!sc->init_done) {
		ret = -EPERM;
		goto out;
	}
	ret = sc8547_set_watchdog_code(sc, code);
out:
	mutex_unlock(&sc->lock);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(watchdog_ms);

static ssize_t enable_window_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	const struct sc8547_enable_window *w = &sc->window;

	if (!w->complete)
		return sysfs_emit(buf, "incomplete\n");

	return sysfs_emit(buf,
		"vbus_uv=%u..%u vbat_uv=%u..%u\n",
		w->vbus_min_uv, w->vbus_max_uv,
		w->vbat_min_uv, w->vbat_max_uv);
}
static DEVICE_ATTR_RO(enable_window);

static ssize_t work_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int reg;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &reg);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n",
			  reg & SC8547_CHARGE_MODE ? "bypass" : "2:1");
}

int sc8547_set_manual_mode(struct i2c_client *client, bool bypass)
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
static DEVICE_ATTR_RW(work_mode);

static int sc8547_window_check(struct sc8547_device *sc)
{
	const struct sc8547_enable_window *w = &sc->window;
	int vbus_uv, vbat_uv;

	vbus_uv = sc8547_get_vbus_uv(sc);
	if (vbus_uv < 0)
		return vbus_uv;
	vbat_uv = sc8547_get_vbat_uv(sc);
	if (vbat_uv < 0)
		return vbat_uv;

	if ((u32)vbus_uv < w->vbus_min_uv || (u32)vbus_uv > w->vbus_max_uv ||
	    (u32)vbat_uv < w->vbat_min_uv || (u32)vbat_uv > w->vbat_max_uv)
		return -ERANGE;

	return 0;
}

static int sc8547_enable_preflight(struct sc8547_device *sc)
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

static int sc8547_post_enable_check(struct sc8547_device *sc)
{
	unsigned int reg06, reg0e;
	int ret;

	ret = sc8547_read_status(sc, &reg06, &reg0e);
	if (ret)
		return ret;
	if (!(reg0e & SC8547_ADAPTER_INSERT_STAT) ||
	    !(reg0e & SC8547_VBAT_INSERT_STAT))
		return -ENODEV;
	if (!(reg06 & SC8547_CP_SWITCHING_STAT))
		return -EIO;
	if (sc8547_has_blocking_fault(reg06, reg0e))
		return -EIO;

	return sc8547_window_check(sc);
}

static int sc8547_manual_disable(struct sc8547_device *sc)
{
	unsigned int reg;
	int ret;

	ret = sc8547_set_charge_enabled(sc, false);
	if (ret)
		return ret;
	usleep_range(10000, 20000);

	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &reg);
	if (ret)
		return ret;
	if (reg & SC8547_CHG_EN)
		return -EIO;

	return 0;
}

static ssize_t cp_enable_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct sc8547_device *sc = dev_get_drvdata(dev);
	unsigned int reg;
	int ret;

	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &reg);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", !!(reg & SC8547_CHG_EN));
}

int sc8547_manual_enable(struct i2c_client *client)
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
static DEVICE_ATTR_RW(cp_enable);

static struct attribute *sc8547_experimental_attrs[] = {
	&dev_attr_profile_raw.attr,
	&dev_attr_init_state.attr,
	&dev_attr_apply_init.attr,
	&dev_attr_watchdog_ms.attr,
	&dev_attr_enable_window.attr,
	&dev_attr_work_mode.attr,
	&dev_attr_cp_enable.attr,
	NULL,
};

static umode_t sc8547_experimental_is_visible(struct kobject *kobj,
					      struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct sc8547_device *sc = dev_get_drvdata(dev);

	if (attr == &dev_attr_enable_window.attr ||
	    attr == &dev_attr_work_mode.attr ||
	    attr == &dev_attr_cp_enable.attr) {
		if (!sc || !sc->allow_experimental_cp_enable)
			return 0;
	}

	return attr->mode;
}

static const struct attribute_group sc8547_experimental_attr_group = {
	.name = "sc8547_experimental",
	.attrs = sc8547_experimental_attrs,
	.is_visible = sc8547_experimental_is_visible,
};

static int sc8547_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct sc8547_device *sc;
	unsigned int id, mode, enabled, status;
	const char *role;
	char *psy_name;
	int ret;

	sc = devm_kzalloc(&client->dev, sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;

	sc->dev = &client->dev;
	sc->match_info = device_get_match_data(&client->dev);
	mutex_init(&sc->lock);
	sc->regmap = devm_regmap_init_i2c(client, &sc8547_regmap_config);
	if (IS_ERR(sc->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(sc->regmap),
				     "failed to initialize regmap\n");

	i2c_set_clientdata(client, sc);

	ret = regmap_read(sc->regmap, SC8547_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read device ID\n");

	sc->device_id = id;
	sc->variant = sc8547_detect_variant(sc, sc->device_id);
	if (device_property_read_string(&client->dev, "southchip,role", &role))
		role = "standalone";
	sc->role = role;

	if (sc->match_info && sc->match_info->variant != sc->variant &&
	    sc->variant != SC8547_VARIANT_UNKNOWN)
		dev_warn(&client->dev,
			 "DT compatible suggests %s but device ID 0x%02x identifies %s\n",
			 sc->match_info->name, sc->device_id,
			 sc8547_variant_name(sc->variant));

	sc8547_parse_experimental_profile(sc);
	sc8547_parse_enable_window(sc);

	/* ADC enable alone does not start charge pumping. */
	ret = sc8547_set_adc_enabled(sc, true);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to enable ADC\n");

	psy_name = devm_kasprintf(&client->dev, GFP_KERNEL, "sc8547-%s-%d-%02x",
				   sc->role, client->adapter->nr, client->addr);
	if (!psy_name)
		return -ENOMEM;

	sc->psy_desc.name = psy_name;
	sc->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
	sc->psy_desc.properties = sc8547_psy_props;
	sc->psy_desc.num_properties = ARRAY_SIZE(sc8547_psy_props);
	sc->psy_desc.get_property = sc8547_psy_get_property;
	psy_cfg.drv_data = sc;
	psy_cfg.fwnode = dev_fwnode(&client->dev);

	sc->psy = devm_power_supply_register(&client->dev, &sc->psy_desc, &psy_cfg);
	if (IS_ERR(sc->psy))
		return dev_err_probe(&client->dev, PTR_ERR(sc->psy),
				     "failed to register power supply\n");

	ret = devm_device_add_group(&client->dev, &sc8547_attr_group);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to create bring-up attributes\n");

	if (sc->allow_experimental_control) {
		ret = devm_device_add_group(&client->dev,
					    &sc8547_experimental_attr_group);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to create experimental attributes\n");
		dev_warn(&client->dev,
			 "development-only experimental controls exposed%s\n",
			 sc->allow_experimental_cp_enable ?
			 " including manual single-pump enable" :
			 " (init/watchdog only)");
	}

	ret = regmap_read(sc->regmap, SC8547_REG_CHG_CTRL, &enabled);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_MODE_CTRL, &mode);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_06, &status);
	if (ret)
		return ret;

	dev_info(&client->dev,
		 "%s ID=0x%02x role=%s CP=%s switching=%s mode=%s%s\n",
		 sc8547_variant_name(sc->variant), sc->device_id, sc->role,
		 enabled & SC8547_CHG_EN ? "on" : "off",
		 status & SC8547_CP_SWITCHING_STAT ? "yes" : "no",
		 mode & SC8547_CHARGE_MODE ? "bypass" : "2:1",
		 sc->allow_experimental_control ?
		 " experimental-control" : " telemetry-only");

	if (!sc8547_variant_control_supported(sc->variant))
		dev_warn(&client->dev,
			 "silicon variant is not enabled for experimental writes\n");

	return 0;
}

static void sc8547_shutdown(struct i2c_client *client)
{
	struct sc8547_device *sc = i2c_get_clientdata(client);

	if (!sc || !sc->allow_experimental_control)
		return;

	/* Best-effort fail closed; never leave development CP/WDT active. */
	sc8547_fail_closed(sc);
}

static const struct of_device_id sc8547_of_match[] = {
	{ .compatible = "southchip,sc8547", .data = &sc8547_info },
	{ .compatible = "southchip,sc8547a", .data = &sc8547a_info },
	/* Downstream aliases are accepted only to simplify bring-up. */
	{ .compatible = "oplus,sc8547a", .data = &sc8547a_info },
	{ .compatible = "slave_vphy_sc8547", .data = &sc8547_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8547_of_match);

static const struct i2c_device_id sc8547_i2c_id[] = {
	{ "sc8547" },
	{ "sc8547a" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sc8547_i2c_id);

static struct i2c_driver sc8547_driver = {
	.driver = {
		.name = "sc8547",
		.of_match_table = sc8547_of_match,
	},
	.probe = sc8547_probe,
	.shutdown = sc8547_shutdown,
	.id_table = sc8547_i2c_id,
};
module_i2c_driver(sc8547_driver);

MODULE_DESCRIPTION("Southchip SC8547/SC8547A charge-pump bring-up driver");
MODULE_LICENSE("GPL");
