// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dual-SC8547 coordinator for OnePlus Pad Pro (caihong) bring-up.
 *
 * Stage 5A is read-only. Physical-chip state is obtained through the SC8547
 * driver's shared safety API so the virtual layer does not duplicate register
 * interpretation or bypass physical-driver locking.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "sc8547_api.h"

struct sc8547_dual {
	struct device *dev;
	struct device_node *primary_np;
	struct device_node *secondary_np;
};

static const char *sc8547_dual_variant_name(enum sc8547_api_variant variant)
{
	switch (variant) {
	case SC8547_API_VARIANT_SC8547:
		return "sc8547";
	case SC8547_API_VARIANT_SC8547A:
		return "sc8547a";
	case SC8547_API_VARIANT_SC8547D:
		return "sc8547d";
	default:
		return "unknown";
	}
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

static int sc8547_dual_get_states(struct sc8547_dual *dual,
				  struct i2c_client **primary,
				  struct i2c_client **secondary,
				  struct sc8547_state *pstate,
				  struct sc8547_state *sstate)
{
	int ret;

	ret = sc8547_dual_get_clients(dual, primary, secondary);
	if (ret)
		return ret;

	ret = sc8547_get_state(*primary, pstate);
	if (ret)
		goto err_put;
	ret = sc8547_get_state(*secondary, sstate);
	if (ret)
		goto err_put;

	return 0;

err_put:
	sc8547_dual_put_clients(*primary, *secondary);
	return ret;
}

static ssize_t peer_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct sc8547_dual *dual = dev_get_drvdata(dev);
	struct i2c_client *primary, *secondary;
	struct sc8547_state p = {}, s = {};
	ssize_t len;
	int ret;

	ret = sc8547_dual_get_states(dual, &primary, &secondary, &p, &s);
	if (ret)
		return sysfs_emit(buf, "unavailable\n");

	len = sysfs_emit(buf,
		"primary=%s role=primary id=0x%02x variant=%s secondary=%s role=secondary id=0x%02x variant=%s\n",
		dev_name(&primary->dev), p.device_id,
		sc8547_dual_variant_name(p.variant),
		dev_name(&secondary->dev), s.device_id,
		sc8547_dual_variant_name(s.variant));
	sc8547_dual_put_clients(primary, secondary);
	return len;
}
static DEVICE_ATTR_RO(peer);

static ssize_t pair_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct sc8547_dual *dual = dev_get_drvdata(dev);
	struct i2c_client *primary, *secondary;
	struct sc8547_state p = {}, s = {};
	size_t len = 0;
	int ret;

	ret = sc8547_dual_get_states(dual, &primary, &secondary, &p, &s);
	if (ret)
		return ret;

	len += sysfs_emit_at(buf, len,
		"primary dev=%s id=0x%02x variant=%s initialized=%u stage4_authorized=%u enable=%u switching=%u mode=%s blocking_fault=%u vbus_uv=%d vbat_uv=%d ibus_ua=%d\n",
		dev_name(&primary->dev), p.device_id,
		sc8547_dual_variant_name(p.variant), p.initialized,
		p.stage4_authorized, p.enabled, p.switching,
		p.bypass ? "bypass" : "2:1", p.blocking_fault,
		p.vbus_uv, p.vbat_uv, p.ibus_ua);
	len += sysfs_emit_at(buf, len,
		"secondary dev=%s id=0x%02x variant=%s initialized=%u stage4_authorized=%u enable=%u switching=%u mode=%s blocking_fault=%u vbus_uv=%d vbat_uv=%d ibus_ua=%d\n",
		dev_name(&secondary->dev), s.device_id,
		sc8547_dual_variant_name(s.variant), s.initialized,
		s.stage4_authorized, s.enabled, s.switching,
		s.bypass ? "bypass" : "2:1", s.blocking_fault,
		s.vbus_uv, s.vbat_uv, s.ibus_ua);
	len += sysfs_emit_at(buf, len, "aggregate_ibus_ua=%d\n",
			     p.ibus_ua + s.ibus_ua);

	sc8547_dual_put_clients(primary, secondary);
	return len;
}
static DEVICE_ATTR_RO(pair_state);

static ssize_t aggregate_ibus_ua_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sc8547_dual *dual = dev_get_drvdata(dev);
	struct i2c_client *primary, *secondary;
	struct sc8547_state p = {}, s = {};
	int ret;

	ret = sc8547_dual_get_states(dual, &primary, &secondary, &p, &s);
	if (ret)
		return ret;

	sc8547_dual_put_clients(primary, secondary);
	return sysfs_emit(buf, "%d\n", p.ibus_ua + s.ibus_ua);
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
		 "read-only SC8547 dual-pump coordinator ready; physical state uses shared API\n");
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

MODULE_DESCRIPTION("Dual SC8547 charge-pump bring-up coordinator");
MODULE_LICENSE("GPL");
