// SPDX-License-Identifier: GPL-2.0-only
/*
 * SmartSens SC820CS bring-up driver for OnePlus Pad Pro (caihong).
 *
 * This first-stage driver is intentionally probe-only.  It powers the sensor,
 * checks the chip ID and registers a V4L2 sensor subdevice, but refuses stream
 * enable until the Caihong-specific mode sequence has been recovered and
 * validated.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define SC820CS_XVCLK_FREQ		19200000

#define SC820CS_REG_CHIP_ID_H		0x3107
#define SC820CS_REG_CHIP_ID_L		0x3108
#define SC820CS_CHIP_ID			0xd154

#define SC820CS_NATIVE_WIDTH		3264
#define SC820CS_NATIVE_HEIGHT		2448
#define SC820CS_NUM_DATA_LANES		4

struct sc820cs {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[3];
};

static const char * const sc820cs_supply_names[] = {
	"dovdd",
	"avdd",
	"dvdd",
};

static inline struct sc820cs *to_sc820cs(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sc820cs, sd);
}

static int sc820cs_read8(struct sc820cs *sc820cs, u16 reg, u8 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sc820cs->sd);
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.len = sizeof(addr_buf),
			.buf = addr_buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = val,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

static int sc820cs_power_on(struct sc820cs *sc820cs)
{
	int ret;

	/* Keep the sensor in reset while rails and the input clock settle. */
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);

	ret = regulator_bulk_enable(ARRAY_SIZE(sc820cs->supplies),
				    sc820cs->supplies);
	if (ret)
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to enable supplies\n");

	ret = clk_prepare_enable(sc820cs->xvclk);
	if (ret) {
		dev_err(sc820cs->dev, "failed to enable xvclk: %d\n", ret);
		goto disable_supplies;
	}

	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 0);
	usleep_range(5000, 7000);

	return 0;

disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(sc820cs->supplies),
			       sc820cs->supplies);
	return ret;
}

static void sc820cs_power_off(struct sc820cs *sc820cs)
{
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);
	clk_disable_unprepare(sc820cs->xvclk);
	regulator_bulk_disable(ARRAY_SIZE(sc820cs->supplies),
			       sc820cs->supplies);
}

static int sc820cs_identify(struct sc820cs *sc820cs)
{
	u8 id_h, id_l;
	u16 id;
	int ret;

	ret = sc820cs_read8(sc820cs, SC820CS_REG_CHIP_ID_H, &id_h);
	if (ret)
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to read chip-id high byte\n");

	ret = sc820cs_read8(sc820cs, SC820CS_REG_CHIP_ID_L, &id_l);
	if (ret)
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to read chip-id low byte\n");

	id = ((u16)id_h << 8) | id_l;
	if (id != SC820CS_CHIP_ID) {
		dev_err(sc820cs->dev, "chip ID mismatch: got 0x%04x, expected 0x%04x\n",
			id, SC820CS_CHIP_ID);
		return -ENODEV;
	}

	dev_info(sc820cs->dev, "SC820CS detected, chip ID 0x%04x\n", id);
	return 0;
}

static void sc820cs_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = SC820CS_NATIVE_WIDTH;
	fmt->height = SC820CS_NATIVE_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
}

static int sc820cs_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sc820cs_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = SC820CS_NATIVE_WIDTH;
	fse->max_width = SC820CS_NATIVE_WIDTH;
	fse->min_height = SC820CS_NATIVE_HEIGHT;
	fse->max_height = SC820CS_NATIVE_HEIGHT;
	return 0;
}

static int sc820cs_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	if (fmt->pad)
		return -EINVAL;

	sc820cs_fill_format(&fmt->format);
	return 0;
}

static int sc820cs_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	if (fmt->pad)
		return -EINVAL;

	sc820cs_fill_format(&fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;

	return 0;
}

static int sc820cs_set_stream(struct v4l2_subdev *sd, int enable)
{
	if (!enable)
		return 0;

	dev_warn_ratelimited(to_sc820cs(sd)->dev,
		"streaming is disabled in the probe-only bring-up driver\n");
	return -EOPNOTSUPP;
}

static const struct v4l2_subdev_video_ops sc820cs_video_ops = {
	.s_stream = sc820cs_set_stream,
};

static const struct v4l2_subdev_pad_ops sc820cs_pad_ops = {
	.enum_mbus_code = sc820cs_enum_mbus_code,
	.enum_frame_size = sc820cs_enum_frame_size,
	.get_fmt = sc820cs_get_fmt,
	.set_fmt = sc820cs_set_fmt,
};

static const struct v4l2_subdev_ops sc820cs_subdev_ops = {
	.video = &sc820cs_video_ops,
	.pad = &sc820cs_pad_ops,
};

static const struct media_entity_operations sc820cs_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int sc820cs_get_resources(struct sc820cs *sc820cs)
{
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	unsigned long rate;
	unsigned int i;
	int ret;

	sc820cs->xvclk = devm_clk_get(sc820cs->dev, "xvclk");
	if (IS_ERR(sc820cs->xvclk))
		return dev_err_probe(sc820cs->dev, PTR_ERR(sc820cs->xvclk),
				     "failed to get xvclk\n");

	rate = clk_get_rate(sc820cs->xvclk);
	if (rate != SC820CS_XVCLK_FREQ)
		dev_warn(sc820cs->dev,
			 "xvclk is %lu Hz; Caihong downstream uses %u Hz\n",
			 rate, SC820CS_XVCLK_FREQ);

	sc820cs->reset_gpio = devm_gpiod_get_optional(sc820cs->dev, "reset",
						       GPIOD_OUT_HIGH);
	if (IS_ERR(sc820cs->reset_gpio))
		return dev_err_probe(sc820cs->dev,
				     PTR_ERR(sc820cs->reset_gpio),
				     "failed to get reset GPIO\n");

	for (i = 0; i < ARRAY_SIZE(sc820cs_supply_names); i++)
		sc820cs->supplies[i].supply = sc820cs_supply_names[i];

	ret = devm_regulator_bulk_get(sc820cs->dev,
				       ARRAY_SIZE(sc820cs->supplies),
				       sc820cs->supplies);
	if (ret)
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to get supplies\n");

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(sc820cs->dev), NULL);
	if (!endpoint)
		return dev_err_probe(sc820cs->dev, -EINVAL,
				     "missing CSI-2 endpoint\n");

	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to parse CSI-2 endpoint\n");

	if (ep.bus.mipi_csi2.num_data_lanes != SC820CS_NUM_DATA_LANES)
		return dev_err_probe(sc820cs->dev, -EINVAL,
				     "expected %u CSI-2 lanes, DT has %u\n",
				     SC820CS_NUM_DATA_LANES,
				     ep.bus.mipi_csi2.num_data_lanes);

	return 0;
}

static int sc820cs_probe(struct i2c_client *client)
{
	struct sc820cs *sc820cs;
	int ret;

	sc820cs = devm_kzalloc(&client->dev, sizeof(*sc820cs), GFP_KERNEL);
	if (!sc820cs)
		return -ENOMEM;

	sc820cs->dev = &client->dev;

	ret = sc820cs_get_resources(sc820cs);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&sc820cs->sd, client, &sc820cs_subdev_ops);

	ret = sc820cs_power_on(sc820cs);
	if (ret)
		return ret;

	ret = sc820cs_identify(sc820cs);
	sc820cs_power_off(sc820cs);
	if (ret)
		return ret;

	sc820cs->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sc820cs->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sc820cs->sd.entity.ops = &sc820cs_entity_ops;
	sc820cs->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sc820cs->sd.entity, 1, &sc820cs->pad);
	if (ret)
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to initialize media entity\n");

	ret = v4l2_async_register_subdev_sensor(&sc820cs->sd);
	if (ret) {
		media_entity_cleanup(&sc820cs->sd.entity);
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to register V4L2 subdevice\n");
	}

	dev_info(sc820cs->dev,
		 "probe-only V4L2 subdevice registered; streaming intentionally disabled\n");
	return 0;
}

static void sc820cs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id sc820cs_of_match[] = {
	{ .compatible = "smartsens,sc820cs" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc820cs_of_match);

static struct i2c_driver sc820cs_i2c_driver = {
	.driver = {
		.name = "sc820cs",
		.of_match_table = sc820cs_of_match,
	},
	.probe = sc820cs_probe,
	.remove = sc820cs_remove,
};
module_i2c_driver(sc820cs_i2c_driver);

MODULE_DESCRIPTION("SmartSens SC820CS probe-only V4L2 sensor driver");
MODULE_LICENSE("GPL");
