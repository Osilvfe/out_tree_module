// SPDX-License-Identifier: GPL-2.0-only
/*
 * Southchip SC8547/SC8547A charge-pump bring-up driver.
 *
 * Standalone mainline-style port for OnePlus Pad Pro (caihong). The default
 * path remains conservative: probe enables ADC conversion and exposes
 * telemetry/state, but never starts the charge pump or changes protection
 * thresholds.
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>

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

/* ADC scales converted from the Oplus downstream register definitions. */
#define SC8547_IBUS_UA_PER_LSB		1875
#define SC8547_VBUS_UV_PER_LSB		3750
#define SC8547_VAC_UV_PER_LSB		5000
#define SC8547_VOUT_UV_PER_LSB		1250
#define SC8547_VBAT_UV_PER_LSB		1250
#define SC8547_TDIE_MC_PER_LSB		500

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

struct sc8547_device {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	const struct sc8547_chip_info *match_info;
	const char *role;
	enum sc8547_variant variant;
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

/*
 * Common masked control helpers. These intentionally preserve variant-specific
 * low bits. They are not exposed as writable userspace controls in this
 * revision; later bring-up commits can use them after protection setup is
 * validated on hardware.
 */
static int __maybe_unused sc8547_set_charge_enabled(struct sc8547_device *sc,
						     bool enable)
{
	return regmap_update_bits(sc->regmap, SC8547_REG_CHG_CTRL,
				 SC8547_CHG_EN, enable ? SC8547_CHG_EN : 0);
}

static int __maybe_unused sc8547_set_work_mode(struct sc8547_device *sc,
					       bool bypass)
{
	return regmap_update_bits(sc->regmap, SC8547_REG_MODE_CTRL,
				 SC8547_CHARGE_MODE,
				 bypass ? SC8547_CHARGE_MODE : 0);
}

static int __maybe_unused sc8547_set_adc_enabled(struct sc8547_device *sc,
						  bool enable)
{
	return regmap_update_bits(sc->regmap, SC8547_REG_ADC_CTRL,
				 SC8547_ADC_EN, enable ? SC8547_ADC_EN : 0);
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

	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_06, &reg06);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_0E, &reg0e);
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

	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_06, &reg06);
	if (ret)
		return ret;
	ret = regmap_read(sc->regmap, SC8547_REG_STATUS_0E, &reg0e);
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

	/* Common charge-pump/ADC register map only; do not touch UFCS buffers. */
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
		"bat_ocp code=%u header_ma=%u\n",
		bat_ocp_code,
		SC8547_BAT_OCP_BASE_MA + bat_ocp_code * SC8547_BAT_OCP_STEP_MA);
	len += sysfs_emit_at(buf, len,
		"ac_ovp code=%u header_formula_mv=%u\n",
		ac_ovp_code,
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
		FIELD_GET(SC8547_SS_TIMEOUT_MASK, r08),
		FIELD_GET(SC8547_PMID2OUT_UVP_MASK, r0d),
		FIELD_GET(SC8547_PMID2OUT_OVP_MASK, r0d));
	if (watchdog_ms >= 0)
		len += sysfs_emit_at(buf, len, "watchdog_ms=%d\n", watchdog_ms);
	else
		len += sysfs_emit_at(buf, len, "watchdog_ms=reserved_code_%u\n",
				     FIELD_GET(SC8547_WATCHDOG_MASK, r09));
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
	psy_cfg.of_node = client->dev.of_node;

	sc->psy = devm_power_supply_register(&client->dev, &sc->psy_desc, &psy_cfg);
	if (IS_ERR(sc->psy))
		return dev_err_probe(&client->dev, PTR_ERR(sc->psy),
				     "failed to register power supply\n");

	ret = devm_device_add_group(&client->dev, &sc8547_attr_group);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to create bring-up attributes\n");

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
		 "%s ID=0x%02x role=%s CP=%s switching=%s mode=%s (telemetry-only)\n",
		 sc8547_variant_name(sc->variant), sc->device_id, sc->role,
		 enabled & SC8547_CHG_EN ? "on" : "off",
		 status & SC8547_CP_SWITCHING_STAT ? "yes" : "no",
		 mode & SC8547_CHARGE_MODE ? "bypass" : "2:1");

	if (sc->variant == SC8547_VARIANT_UNKNOWN)
		dev_warn(&client->dev,
			 "unlisted SC8547-family device ID; keep control path disabled until identified\n");

	return 0;
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
	.id_table = sc8547_i2c_id,
};
module_i2c_driver(sc8547_driver);

MODULE_DESCRIPTION("Southchip SC8547/SC8547A charge-pump bring-up driver");
MODULE_LICENSE("GPL");
