// SPDX-License-Identifier: GPL-2.0-only
/*
 * SmartSens SC1320CS bring-up driver for OnePlus Pad Pro (caihong).
 *
 * This first stage deliberately performs only a powered, read-only ID probe.
 * Caihong's rear sensor has been confirmed at Linux 7-bit address 0x36. The
 * remaining candidate addresses are retained as a diagnostic fallback for
 * board revisions, while the DT client starts at the confirmed address.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#define SC1320CS_XVCLK_FREQ		19200000
#define SC1320CS_CHIP_ID_REG_H		0x3107
#define SC1320CS_CHIP_ID_REG_L		0x3108
#define SC1320CS_CHIP_ID			0xc658
#define SC1320CS_CONFIRMED_ADDR	0x36

static const unsigned short sc1320cs_probe_addresses[] = {
	SC1320CS_CONFIRMED_ADDR, 0x10, 0x20, 0x21, 0x30, 0x31, 0x37, 0x3c,
};

static const unsigned int sc1320cs_supply_loads[] = {
	300000, /* VIO / L4B */
	600000, /* VANA / L16B */
	600000, /* VDIG / L2G */
};

struct sc1320cs {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[3];
	bool powered;
};

static const char * const sc1320cs_supply_names[] = {
	"dovdd", "avdd", "dvdd",
};

static inline struct sc1320cs *to_sc1320cs(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sc1320cs, sd);
}

static int sc1320cs_power_on(struct sc1320cs *sensor)
{
	unsigned int i;
	int ret;

	if (sensor->powered)
		return 0;

	/* Keep reset asserted while rails and MCLK settle. */
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		ret = regulator_set_load(sensor->supplies[i].consumer,
					 sc1320cs_supply_loads[i]);
		if (ret)
			return dev_err_probe(sensor->dev, ret,
					     "failed to set %s load\n",
					     sc1320cs_supply_names[i]);
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(sensor->supplies),
				    sensor->supplies);
	if (ret)
		return dev_err_probe(sensor->dev, ret, "failed to enable supplies\n");

	ret = clk_prepare_enable(sensor->xvclk);
	if (ret)
		goto disable_supplies;

	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(10000, 12000);
	sensor->powered = true;
	return 0;

disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
	return ret;
}

static void sc1320cs_power_off(struct sc1320cs *sensor)
{
	if (!sensor->powered)
		return;

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->xvclk);
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
	sensor->powered = false;
}

static int sc1320cs_read_id(struct sc1320cs *sensor, unsigned int *id)
{
	u64 high, low;
	int ret;

	ret = cci_read(sensor->regmap, CCI_REG8(SC1320CS_CHIP_ID_REG_H),
		       &high, NULL);
	if (ret)
		return ret;
	ret = cci_read(sensor->regmap, CCI_REG8(SC1320CS_CHIP_ID_REG_L),
		       &low, NULL);
	if (ret)
		return ret;

	*id = ((unsigned int)high << 8) | (unsigned int)low;
	return 0;
}

static int sc1320cs_find_address(struct i2c_client *client,
				 struct sc1320cs *sensor)
{
	unsigned int id;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(sc1320cs_probe_addresses); i++) {
		client->addr = sc1320cs_probe_addresses[i];
		ret = sc1320cs_read_id(sensor, &id);
		if (!ret && id == SC1320CS_CHIP_ID) {
			dev_info(sensor->dev,
				 "SC1320CS detected at 0x%02x, chip ID 0x%04x\n",
				 client->addr, id);
			return 0;
		}
		if (ret != -ENXIO && ret != -EREMOTEIO && ret != -EIO)
			dev_dbg(sensor->dev, "address 0x%02x read failed: %d\n",
				client->addr, ret);
		else
			dev_dbg(sensor->dev, "address 0x%02x did not acknowledge\n",
				client->addr);
	}

	client->addr = SC1320CS_CONFIRMED_ADDR;
	return -ENODEV;
}

static const struct media_entity_operations sc1320cs_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_ops sc1320cs_subdev_ops = {
};

static int sc1320cs_probe(struct i2c_client *client)
{
	struct sc1320cs *sensor;
	unsigned int i;
	int ret;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &client->dev;
	sensor->xvclk = devm_clk_get(&client->dev, "xvclk");
	if (IS_ERR(sensor->xvclk))
		return dev_err_probe(&client->dev, PTR_ERR(sensor->xvclk),
				     "failed to get MCLK1\n");

	sensor->reset_gpio = devm_gpiod_get(&client->dev, "reset",
					   GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO82\n");

	for (i = 0; i < ARRAY_SIZE(sc1320cs_supply_names); i++)
		sensor->supplies[i].supply = sc1320cs_supply_names[i];
	ret = devm_regulator_bulk_get(&client->dev,
				      ARRAY_SIZE(sensor->supplies), sensor->supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to get rear-camera supplies\n");

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(sensor->regmap),
				     "failed to initialize CCI regmap\n");

	v4l2_i2c_subdev_init(&sensor->sd, client, &sc1320cs_subdev_ops);
	ret = sc1320cs_power_on(sensor);
	if (ret)
		return ret;

	ret = sc1320cs_find_address(client, sensor);
	sc1320cs_power_off(sensor);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "SC1320CS was not found on CCI0 master 1\n");

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->sd.entity.ops = &sc1320cs_entity_ops;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to initialize rear-camera media entity\n");

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret) {
		media_entity_cleanup(&sensor->sd.entity);
		return dev_err_probe(&client->dev, ret,
				     "failed to register rear-camera subdevice\n");
	}

	dev_info(sensor->dev,
		 "rear-camera probe-only subdevice registered at 0x%02x\n",
		 client->addr);
	return 0;
}

static void sc1320cs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id sc1320cs_of_match[] = {
	{ .compatible = "smartsens,sc1320cs" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc1320cs_of_match);

static struct i2c_driver sc1320cs_i2c_driver = {
	.driver = {
		.name = "sc1320cs",
		.of_match_table = sc1320cs_of_match,
	},
	.probe = sc1320cs_probe,
	.remove = sc1320cs_remove,
};
module_i2c_driver(sc1320cs_i2c_driver);

MODULE_DESCRIPTION("SmartSens SC1320CS read-only probe V4L2 sensor driver");
MODULE_LICENSE("GPL");
