// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only dual-SC8547 coordinator for OnePlus Pad Pro (caihong) bring-up.
 *
 * Stage 5A deliberately performs no writes. It resolves two SC8547-family I2C
 * devices through explicit DT phandles and exposes paired/aggregate telemetry.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/string.h>
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
#define SC8547_FAULT_MASK_0E		GENMASK(7, 2)

#define SC8547_REG_IBUS_ADC_H		0x13
#define SC8547_REG_VBUS_ADC_H		0x15
#define SC8547_REG_VBAT_ADC_H		0x1b
#define SC8547_REG_DEVICE_ID		0x36

#define SC8547_ADC_12BIT_H_MASK		GENMASK(3, 0)
#define SC8547_IBUS_UA_PER_LSB		1875
#define SC8547_VBUS_UV_PER_LSB		3750
#define SC8547_VBAT_UV_PER_LSB		1250

#define SC8547A_DEVICE_ID		0x67
#define SC8547D_DEVICE_ID		0x49

struct sc8547_dual {
	struct device *dev;
	struct device_node *primary_np;
	struct device_node *secondary_np;
};

struct sc8547_dual_sample {
	u8 id;
	u8 reg06;
	u8 reg07;
	u8 reg09;
	u8 reg0e;
	int vbus_uv;
	int vbat_uv;
	int ibus_ua;
};

static const char *sc8547_dual_variant_name(u8 id)
{
	if (id == SC8547A_DEVICE_ID)
		return "sc8547a";
	if (id == SC8547D_DEVICE_ID)
		return "sc8547d";

	return "sc8547-family";
}

static bool sc8547_dual_blocking_fault(const struct sc8547_dual_sample *s)
{
	return !!(s->reg06 & (SC8547_TSHUT_STAT |
			      SC8547_VBUS_ERRORLO_STAT |
			      SC8547_VBUS_ERRORHI_STAT)) ||
	       !!(s->reg0e & SC8547_FAULT_MASK_0E);
}

static int sc8547_dual_read_byte(struct i2c_client *client, u8 reg, u8 *val)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static int sc8547_dual_read_adc(struct i2c_client *client, u8 reg, int scale)
{
	s32 hi, lo;

	hi = i2c_smbus_read_byte_data(client, reg);
	if (hi < 0)
		return hi;
	lo = i2c_smbus_read_byte_data(client, reg + 1);
	if (lo < 0)
		return lo;

	return ((((u8)hi & SC8547_ADC_12BIT_H_MASK) << 8) | (u8)lo) * scale;
}

static int sc8547_dual_sample(struct i2c_client *client,
			      struct sc8547_dual_sample *s)
{
	int ret;

	ret = sc8547_dual_read_byte(client, SC8547_REG_DEVICE_ID, &s->id);
	if (ret)
		return ret;
	ret = sc8547_dual_read_byte(client, SC8547_REG_STATUS_06, &s->reg06);
	if (ret)
		return ret;
	ret = sc8547_dual_read_byte(client, SC8547_REG_CHG_CTRL, &s->reg07);
	if (ret)
		return ret;
	ret = sc8547_dual_read_byte(client, SC8547_REG_MODE_CTRL, &s->reg09);
	if (ret)
		return ret;
	ret = sc8547_dual_read_byte(client, SC8547_REG_STATUS_0E, &s->reg0e);
	if (ret)
		return ret;

	s->vbus_uv = sc8547_dual_read_adc(client, SC8547_REG_VBUS_ADC_H,
					  SC8547_VBUS_UV_PER_LSB);
	if (s->vbus_uv < 0)
		return s->vbus_uv;
	s->vbat_uv = sc8547_dual_read_adc(client, SC8547_REG_VBAT_ADC_H,
					  SC8547_VBAT_UV_PER_LSB);
	if (s->vbat_uv < 0)
		return s->vbat_uv;
	s->ibus_ua = sc8547_dual_read_adc(client, SC8547_REG_IBUS_ADC_H,
					  SC8547_IBUS_UA_PER_LSB);
	if (s->ibus_ua < 0)
		return s->ibus_ua;

	return 0;
}

static bool sc8547_dual_client_bound(struct i2c_client *client)
{
	return client->dev.driver &&
	       !strcmp(client->dev.driver->name, "sc8547");
}

static bool sc8547_dual_role_matches(struct i2c_client *client,
				     const char *expected)
{
	const char *role;

	if (device_property_read_string(&client->dev, "southchip,role", &role))
		return false;

	return !strcmp(role, expected);
}

static int sc8547_dual_get_clients(struct sc8547_dual *dual,
				   struct i2c_client **primary,
				   struct i2c_client **secondary)
{
	struct i2c_client *p, *s;

	p = of_find_i2c_device_by_node(dual->primary_np);
	if (!p)
		return -ENODEV;

	s = of_find_i2c_device_by_node(dual->secondary_np);
	if (!s) {
		put_device(&p->dev);
		return -ENODEV;
	}

	if (p == s || !sc8547_dual_client_bound(p) ||
	    !sc8547_dual_client_bound(s) ||
	    !sc8547_dual_role_matches(p, "primary") ||
	    !sc8547_dual_role_matches(s, "secondary")) {
		put_device(&s->dev);
		put_device(&p->dev);
		return -ENODEV;
	}

	*primary = p;
	*secondary = s;
	return 0;
}

static void sc8547_dual_put_clients(struct i2c_client *primary,
				    struct i2c_client *secondary)
{
	put_device(&secondary->dev);
	put_device(&primary->dev);
}

static ssize_t peer_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct sc8547_dual *dual = dev_get_drvdata(dev);
	struct i2c_client *primary, *secondary;
	struct sc8547_dual_sample p = {}, s = {};
	ssize_t len;
	int ret;

	ret = sc8547_dual_get_clients(dual, &primary, &secondary);
	if (ret)
		return sysfs_emit(buf, "unavailable\n");

	ret = sc8547_dual_read_byte(primary, SC8547_REG_DEVICE_ID, &p.id);
	if (ret)
		goto out_err;
	ret = sc8547_dual_read_byte(secondary, SC8547_REG_DEVICE_ID, &s.id);
	if (ret)
		goto out_err;

	len = sysfs_emit(buf,
		"primary=%s role=primary id=0x%02x variant=%s secondary=%s role=secondary id=0x%02x variant=%s\n",
		dev_name(&primary->dev), p.id, sc8547_dual_variant_name(p.id),
		dev_name(&secondary->dev), s.id, sc8547_dual_variant_name(s.id));
	sc8547_dual_put_clients(primary, secondary);
	return len;

out_err:
	sc8547_dual_put_clients(primary, secondary);
	return ret;
}
static DEVICE_ATTR_RO(peer);

static ssize_t pair_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct sc8547_dual *dual = dev_get_drvdata(dev);
	struct i2c_client *primary, *secondary;
	struct sc8547_dual_sample p = {}, s = {};
	size_t len = 0;
	int ret;

	ret = sc8547_dual_get_clients(dual, &primary, &secondary);
	if (ret)
		return ret;

	ret = sc8547_dual_sample(primary, &p);
	if (ret)
		goto out;
	ret = sc8547_dual_sample(secondary, &s);
	if (ret)
		goto out;

	len += sysfs_emit_at(buf, len,
		"primary dev=%s id=0x%02x variant=%s enable=%u switching=%u mode=%s blocking_fault=%u raw06=0x%02x raw0e=0x%02x vbus_uv=%d vbat_uv=%d ibus_ua=%d\n",
		dev_name(&primary->dev), p.id, sc8547_dual_variant_name(p.id),
		!!(p.reg07 & SC8547_CHG_EN),
		!!(p.reg06 & SC8547_CP_SWITCHING_STAT),
		p.reg09 & SC8547_CHARGE_MODE ? "bypass" : "2:1",
		sc8547_dual_blocking_fault(&p), p.reg06, p.reg0e,
		p.vbus_uv, p.vbat_uv, p.ibus_ua);
	len += sysfs_emit_at(buf, len,
		"secondary dev=%s id=0x%02x variant=%s enable=%u switching=%u mode=%s blocking_fault=%u raw06=0x%02x raw0e=0x%02x vbus_uv=%d vbat_uv=%d ibus_ua=%d\n",
		dev_name(&secondary->dev), s.id, sc8547_dual_variant_name(s.id),
		!!(s.reg07 & SC8547_CHG_EN),
		!!(s.reg06 & SC8547_CP_SWITCHING_STAT),
		s.reg09 & SC8547_CHARGE_MODE ? "bypass" : "2:1",
		sc8547_dual_blocking_fault(&s), s.reg06, s.reg0e,
		s.vbus_uv, s.vbat_uv, s.ibus_ua);
	len += sysfs_emit_at(buf, len,
		"aggregate_ibus_ua=%d\n", p.ibus_ua + s.ibus_ua);

out:
	sc8547_dual_put_clients(primary, secondary);
	return ret ? ret : (ssize_t)len;
}
static DEVICE_ATTR_RO(pair_state);

static ssize_t aggregate_ibus_ua_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sc8547_dual *dual = dev_get_drvdata(dev);
	struct i2c_client *primary, *secondary;
	int primary_ua, secondary_ua;
	int ret;

	ret = sc8547_dual_get_clients(dual, &primary, &secondary);
	if (ret)
		return ret;

	primary_ua = sc8547_dual_read_adc(primary, SC8547_REG_IBUS_ADC_H,
					  SC8547_IBUS_UA_PER_LSB);
	if (primary_ua < 0) {
		ret = primary_ua;
		goto out;
	}
	secondary_ua = sc8547_dual_read_adc(secondary, SC8547_REG_IBUS_ADC_H,
					    SC8547_IBUS_UA_PER_LSB);
	if (secondary_ua < 0) {
		ret = secondary_ua;
		goto out;
	}

	ret = sysfs_emit(buf, "%d\n", primary_ua + secondary_ua);

out:
	sc8547_dual_put_clients(primary, secondary);
	return ret;
}
static DEVICE_ATTR_RO(aggregate_ibus_ua);

static struct attribute *sc8547_dual_attrs[] = {
	&dev_attr_peer.attr,
	&dev_attr_pair_state.attr,
	&dev_attr_aggregate_ibus_ua.attr,
	NULL,
};

static const struct attribute_group sc8547_dual_attr_group = {
	.name = "sc8547_dual",
	.attrs = sc8547_dual_attrs,
};

static void sc8547_dual_put_node(void *data)
{
	of_node_put(data);
}

static int sc8547_dual_probe(struct platform_device *pdev)
{
	struct sc8547_dual *dual;
	int ret;

	dual = devm_kzalloc(&pdev->dev, sizeof(*dual), GFP_KERNEL);
	if (!dual)
		return -ENOMEM;

	dual->dev = &pdev->dev;
	dual->primary_np = of_parse_phandle(pdev->dev.of_node,
					   "southchip,primary", 0);
	if (!dual->primary_np)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing southchip,primary phandle\n");
	ret = devm_add_action_or_reset(&pdev->dev, sc8547_dual_put_node,
				       dual->primary_np);
	if (ret)
		return ret;

	dual->secondary_np = of_parse_phandle(pdev->dev.of_node,
					     "southchip,secondary", 0);
	if (!dual->secondary_np)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing southchip,secondary phandle\n");
	ret = devm_add_action_or_reset(&pdev->dev, sc8547_dual_put_node,
				       dual->secondary_np);
	if (ret)
		return ret;

	if (dual->primary_np == dual->secondary_np)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "primary and secondary phandles must differ\n");

	platform_set_drvdata(pdev, dual);

	ret = devm_device_add_group(&pdev->dev, &sc8547_dual_attr_group);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to create dual-pump telemetry group\n");

	dev_info(&pdev->dev,
		 "read-only SC8547 dual-pump coordinator ready; no CP writes are implemented\n");
	return 0;
}

static const struct of_device_id sc8547_dual_of_match[] = {
	{ .compatible = "southchip,sc8547-dual-experimental" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8547_dual_of_match);

static struct platform_driver sc8547_dual_driver = {
	.driver = {
		.name = "sc8547-dual",
		.of_match_table = sc8547_dual_of_match,
	},
	.probe = sc8547_dual_probe,
};
module_platform_driver(sc8547_dual_driver);

MODULE_DESCRIPTION("Read-only dual SC8547 charge-pump coordinator");
MODULE_LICENSE("GPL");
