// SPDX-License-Identifier: GPL-2.0-only
/*
 * SmartSens SC820CS bring-up driver for OnePlus Pad Pro (caihong).
 *
 * The default probe path only powers the sensor long enough to identify it.
 * Streaming is enabled through the normal V4L2 s_stream callback using the
 * Caihong downstream initialization sequence recovered from the vendor tree.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "sc820cs-caihong-regs.h"

#define SC820CS_XVCLK_FREQ		19200000

#define SC820CS_REG_CHIP_ID_H		0x3107
#define SC820CS_REG_CHIP_ID_L		0x3108
#define SC820CS_CHIP_ID			0xd154

#define SC820CS_NATIVE_WIDTH		3264
#define SC820CS_NATIVE_HEIGHT		2448
#define SC820CS_NUM_DATA_LANES		4
#define SC820CS_LINK_FREQ		366000000ULL

static bool keep_power_on_on_probe_failure;
module_param_named(keep_power_on_on_probe_failure,
			   keep_power_on_on_probe_failure, bool, 0644);
MODULE_PARM_DESC(keep_power_on_on_probe_failure,
		 "Keep SC820CS rails, MCLK and reset released after a failed ID read");

static bool allow_probe_failure;
module_param_named(allow_probe_failure, allow_probe_failure, bool, 0644);
MODULE_PARM_DESC(allow_probe_failure,
		 "Register the diagnostic subdevice even when chip ID probing fails");

static const unsigned int sc820cs_supply_loads[] = {
	300000, /* DOVDD / L4B */
	600000, /* AVDD / L16B */
	600000, /* DVDD / L2G */
};

struct sc820cs {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[3];
	struct mutex mutex;
	bool powered;
	bool streaming;
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

static int sc820cs_power_on(struct sc820cs *sc820cs)
{
	unsigned int i;
	int ret;

	if (sc820cs->powered)
		return 0;

	/* Keep the sensor in reset while rails and the input clock settle. */
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);

	for (i = 0; i < ARRAY_SIZE(sc820cs->supplies); i++) {
		ret = regulator_set_load(sc820cs->supplies[i].consumer,
					 sc820cs_supply_loads[i]);
		if (ret)
			return dev_err_probe(sc820cs->dev, ret,
					     "failed to set %s load to %u uA\n",
					     sc820cs_supply_names[i],
					     sc820cs_supply_loads[i]);
	}

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

	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 0);
	usleep_range(10000, 12000);

	dev_dbg(sc820cs->dev, "powered: reset released, xvclk=%lu Hz\n",
		clk_get_rate(sc820cs->xvclk));
	sc820cs->powered = true;

	return 0;

disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(sc820cs->supplies),
			       sc820cs->supplies);
	return ret;
}

static void sc820cs_power_off(struct sc820cs *sc820cs)
{
	if (!sc820cs->powered)
		return;

	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);
	clk_disable_unprepare(sc820cs->xvclk);
	regulator_bulk_disable(ARRAY_SIZE(sc820cs->supplies),
				       sc820cs->supplies);
	sc820cs->powered = false;
}

static int sc820cs_identify(struct sc820cs *sc820cs)
{
	u64 value;
	int ret;

	/* The upstream SmartSens driver reads the two-byte ID in one CCI transfer. */
	ret = cci_read(sc820cs->regmap, CCI_REG16(SC820CS_REG_CHIP_ID_H),
		       &value, NULL);
	if (ret)
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to read chip ID\n");

	if ((u16)value != SC820CS_CHIP_ID) {
		dev_err(sc820cs->dev, "chip ID mismatch: got 0x%04x, expected 0x%04x\n",
			(u16)value, SC820CS_CHIP_ID);
		return -ENODEV;
	}

	dev_info(sc820cs->dev, "SC820CS detected, chip ID 0x%04x\n",
		 (u16)value);
	return 0;
}

static int sc820cs_write_mode(struct sc820cs *sc820cs)
{
	int ret;

	/* The vendor sequence starts with a software reset; leave it time to settle. */
	ret = cci_write(sc820cs->regmap,
			sc820cs_caihong_3264x2448_regs[0].reg,
			sc820cs_caihong_3264x2448_regs[0].val, NULL);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	return cci_multi_reg_write(sc820cs->regmap,
			&sc820cs_caihong_3264x2448_regs[1],
			ARRAY_SIZE(sc820cs_caihong_3264x2448_regs) - 1, NULL);
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

static int sc820cs_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				   struct v4l2_mbus_config *config)
{
	unsigned int i;

	if (pad)
		return -EINVAL;

	config->type = V4L2_MBUS_CSI2_DPHY;
	config->link_freq = SC820CS_LINK_FREQ;
	/* Zero flags means the sensor uses a continuous CSI-2 clock. */
	config->bus.mipi_csi2.flags = 0;
	config->bus.mipi_csi2.clock_lane = 7;
	config->bus.mipi_csi2.num_data_lanes = SC820CS_NUM_DATA_LANES;
	for (i = 0; i < SC820CS_NUM_DATA_LANES; i++)
		config->bus.mipi_csi2.data_lanes[i] = i;

	return 0;
}

static int sc820cs_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct sc820cs *sc820cs = to_sc820cs(sd);
	int ret = 0;

	mutex_lock(&sc820cs->mutex);
	if (!enable) {
		if (sc820cs->streaming)
			ret = cci_write(sc820cs->regmap, CCI_REG8(0x0100), 0x00,
					NULL);
		sc820cs->streaming = false;
		sc820cs_power_off(sc820cs);
		goto unlock;
	}

	if (sc820cs->streaming)
		goto unlock;

	ret = sc820cs_power_on(sc820cs);
	if (ret)
		goto unlock;

	ret = sc820cs_identify(sc820cs);
	if (ret)
		goto power_off;

	ret = sc820cs_write_mode(sc820cs);
	if (ret)
		goto power_off;

	ret = cci_write(sc820cs->regmap, CCI_REG8(0x0100), 0x01, NULL);
	if (ret)
		goto power_off;

	sc820cs->streaming = true;
	dev_info(sc820cs->dev, "SC820CS streaming 3264x2448 RAW10\n");
	goto unlock;

power_off:
	sc820cs_power_off(sc820cs);
unlock:
	mutex_unlock(&sc820cs->mutex);
	return ret;
}

static const struct v4l2_subdev_video_ops sc820cs_video_ops = {
	.s_stream = sc820cs_set_stream,
};

static const struct v4l2_subdev_pad_ops sc820cs_pad_ops = {
	.enum_mbus_code = sc820cs_enum_mbus_code,
	.enum_frame_size = sc820cs_enum_frame_size,
	.get_fmt = sc820cs_get_fmt,
	.set_fmt = sc820cs_set_fmt,
	.get_mbus_config = sc820cs_get_mbus_config,
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
	mutex_init(&sc820cs->mutex);
	sc820cs->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sc820cs->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(sc820cs->regmap),
				     "failed to initialize CCI regmap\n");

	ret = sc820cs_get_resources(sc820cs);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&sc820cs->sd, client, &sc820cs_subdev_ops);

	ret = sc820cs_power_on(sc820cs);
	if (ret)
		return ret;

	ret = sc820cs_identify(sc820cs);
	if (ret && keep_power_on_on_probe_failure) {
		dev_warn(sc820cs->dev,
			 "probe failed; keeping camera power and MCLK on for diagnostics\n");
	} else {
		sc820cs_power_off(sc820cs);
	}
	if (ret && !allow_probe_failure)
		return ret;
	if (ret)
		dev_warn(sc820cs->dev,
			 "registering diagnostic subdevice without a valid chip ID\n");

	sc820cs->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sc820cs->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sc820cs->sd.entity.ops = &sc820cs_entity_ops;
	sc820cs->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sc820cs->sd.entity, 1, &sc820cs->pad);
	if (ret) {
		sc820cs_power_off(sc820cs);
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to initialize media entity\n");
	}

	ret = v4l2_async_register_subdev_sensor(&sc820cs->sd);
	if (ret) {
		media_entity_cleanup(&sc820cs->sd.entity);
		sc820cs_power_off(sc820cs);
		return dev_err_probe(sc820cs->dev, ret,
				     "failed to register V4L2 subdevice\n");
	}

	dev_info(sc820cs->dev,
		 "V4L2 subdevice registered; streaming uses Caihong 3264x2448 mode\n");
	return 0;
}

static void sc820cs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc820cs *sc820cs = to_sc820cs(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	if (sc820cs->streaming)
		cci_write(sc820cs->regmap, CCI_REG8(0x0100), 0x00, NULL);
	sc820cs->streaming = false;
	sc820cs_power_off(sc820cs);
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
