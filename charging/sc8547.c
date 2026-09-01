// SPDX-License-Identifier: GPL-2.0-only
/*
 * Southchip SC8547/SC8547A charge-pump bring-up driver.
 *
 * Standalone mainline-style port for OnePlus Pad Pro (caihong). This initial
 * revision is intentionally conservative: it enables ADC conversion and
 * exposes telemetry/state, but never starts the charge pump or changes the
 * board's protection thresholds.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>

#define SC8547_REG_STATUS_06		0x06
#define SC8547_TSHUT_STAT		BIT(6)
#define SC8547_VBUS_ERRORLO_STAT	BIT(5)
#define SC8547_VBUS_ERRORHI_STAT	BIT(4)
#define SC8547_CP_SWITCHING_STAT	BIT(2)

#define SC8547_REG_CHG_CTRL		0x07
#define SC8547_CHG_EN			BIT(7)

#define SC8547_REG_MODE_CTRL		0x09
#define SC8547_CHARGE_MODE		BIT(7)

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

struct sc8547_device {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	const char *role;
	u8 device_id;
};

static const struct regmap_config sc8547_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

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
		/* POWER_SUPPLY_PROP_TEMP is in tenths of a degree Celsius. */
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

static struct attribute *sc8547_attrs[] = {
	&dev_attr_device_id.attr,
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
	if (device_property_read_string(&client->dev, "southchip,role", &role))
		role = "standalone";
	sc->role = role;

	/* ADC enable alone does not start charge pumping. */
	ret = regmap_update_bits(sc->regmap, SC8547_REG_ADC_CTRL,
				 SC8547_ADC_EN, SC8547_ADC_EN);
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
		 "SC8547-family ID 0x%02x role=%s CP=%s switching=%s mode=%s (telemetry-only)\n",
		 sc->device_id, sc->role,
		 enabled & SC8547_CHG_EN ? "on" : "off",
		 status & SC8547_CP_SWITCHING_STAT ? "yes" : "no",
		 mode & SC8547_CHARGE_MODE ? "bypass" : "2:1");

	if (sc->device_id == SC8547A_DEVICE_ID)
		dev_info(&client->dev, "detected SC8547A\n");
	else if (sc->device_id == SC8547D_DEVICE_ID)
		dev_info(&client->dev, "detected SC8547D-compatible silicon\n");
	else
		dev_info(&client->dev,
			 "unlisted SC8547-family device ID; keeping telemetry-only mode\n");

	return 0;
}

static const struct of_device_id sc8547_of_match[] = {
	{ .compatible = "southchip,sc8547" },
	{ .compatible = "southchip,sc8547a" },
	/* Downstream aliases are accepted only to simplify bring-up. */
	{ .compatible = "oplus,sc8547a" },
	{ .compatible = "slave_vphy_sc8547" },
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
