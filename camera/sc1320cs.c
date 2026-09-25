// SPDX-License-Identifier: GPL-2.0-only
/*
 * SmartSens SC1320CS driver for the OnePlus Pad Pro (caihong).
 *
 * Caihong's rear sensor is connected to CCI0 master 1, CSIPHY1 and MCLK1.
 * Streaming uses the official OnePlus SC1320CS initialization sequence.
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
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "sc1320cs-caihong-regs.h"

#define SC1320CS_XVCLK_FREQ		19200000
#define SC1320CS_CHIP_ID_REG_H		0x3107
#define SC1320CS_CHIP_ID_REG_L		0x3108
#define SC1320CS_CHIP_ID			0xc658
#define SC1320CS_CONFIRMED_ADDR		0x36

#define SC1320CS_NATIVE_WIDTH		4208
#define SC1320CS_NATIVE_HEIGHT		3120
#define SC1320CS_NUM_DATA_LANES		4
#define SC1320CS_LINK_FREQ		600000000ULL
#define SC1320CS_PIXEL_RATE		480000000ULL

#define SC1320CS_HTS			5000
#define SC1320CS_VTS			3200
#define SC1320CS_EXPOSURE_MIN		1
#define SC1320CS_EXPOSURE_MARGIN	4
#define SC1320CS_EXPOSURE_MAX		(SC1320CS_VTS - SC1320CS_EXPOSURE_MARGIN)
#define SC1320CS_EXPOSURE_DEFAULT	3196
#define SC1320CS_ANALOGUE_GAIN_DEFAULT	1024

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
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct mutex mutex;
	bool powered;
	bool streaming;
};

static const char * const sc1320cs_supply_names[] = {
	"dovdd", "avdd", "dvdd",
};

static const s64 sc1320cs_link_freq_menu[] = {
	SC1320CS_LINK_FREQ,
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
		return dev_err_probe(sensor->dev, ret,
				     "failed to enable supplies\n");

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

static int sc1320cs_identify(struct sc1320cs *sensor)
{
	unsigned int id;
	int ret;

	ret = sc1320cs_read_id(sensor, &id);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "failed to read chip ID\n");

	if (id != SC1320CS_CHIP_ID) {
		dev_err(sensor->dev,
			"chip ID mismatch: got 0x%04x, expected 0x%04x\n",
			id, SC1320CS_CHIP_ID);
		return -ENODEV;
	}

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
	}

	client->addr = SC1320CS_CONFIRMED_ADDR;
	return -ENODEV;
}

static int sc1320cs_write_mode(struct sc1320cs *sensor)
{
	int ret;

	ret = cci_write(sensor->regmap,
			sc1320cs_caihong_4208x3120_regs[0].reg,
			sc1320cs_caihong_4208x3120_regs[0].val, NULL);
	if (ret)
		return ret;

	usleep_range(5000, 6000);
	return cci_multi_reg_write(sensor->regmap,
			&sc1320cs_caihong_4208x3120_regs[1],
			ARRAY_SIZE(sc1320cs_caihong_4208x3120_regs) - 1,
			NULL);
}

static int sc1320cs_write_exposure(struct sc1320cs *sensor,
				    unsigned int exposure)
{
	/* SmartSens stores exposure in half-line units across 0x3e00..0x3e02. */
	unsigned int value = exposure << 1;
	const struct cci_reg_sequence regs[] = {
		{ CCI_REG8(0x3e00), (value >> 12) & 0x0f },
		{ CCI_REG8(0x3e01), (value >> 4) & 0xff },
		{ CCI_REG8(0x3e02), (value & 0x0f) << 4 },
	};

	return cci_multi_reg_write(sensor->regmap, regs, ARRAY_SIZE(regs), NULL);
}

static int sc1320cs_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc1320cs *sensor = container_of(ctrl->handler,
					       struct sc1320cs, ctrls);

	if (!sensor->powered)
		return 0;

	if (ctrl->id == V4L2_CID_EXPOSURE)
		return sc1320cs_write_exposure(sensor, ctrl->val);

	return -EINVAL;
}

static const struct v4l2_ctrl_ops sc1320cs_ctrl_ops = {
	.s_ctrl = sc1320cs_set_ctrl,
};

static int sc1320cs_init_controls(struct sc1320cs *sensor)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	int ret;

	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 8);
	if (ret)
		return ret;

	sensor->ctrls.lock = &sensor->mutex;
	link_freq = v4l2_ctrl_new_int_menu(&sensor->ctrls, NULL,
					   V4L2_CID_LINK_FREQ, 0, 0,
					   sc1320cs_link_freq_menu);
	pixel_rate = v4l2_ctrl_new_std(&sensor->ctrls, NULL,
				       V4L2_CID_PIXEL_RATE, 0,
				       SC1320CS_PIXEL_RATE, 1,
				       SC1320CS_PIXEL_RATE);
	hblank = v4l2_ctrl_new_std(&sensor->ctrls, NULL, V4L2_CID_HBLANK,
				   SC1320CS_HTS - SC1320CS_NATIVE_WIDTH,
				   SC1320CS_HTS - SC1320CS_NATIVE_WIDTH, 1,
				   SC1320CS_HTS - SC1320CS_NATIVE_WIDTH);
	vblank = v4l2_ctrl_new_std(&sensor->ctrls, NULL, V4L2_CID_VBLANK,
				   SC1320CS_VTS - SC1320CS_NATIVE_HEIGHT,
				   SC1320CS_VTS - SC1320CS_NATIVE_HEIGHT, 1,
				   SC1320CS_VTS - SC1320CS_NATIVE_HEIGHT);
	sensor->exposure = v4l2_ctrl_new_std(&sensor->ctrls,
					     &sc1320cs_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     SC1320CS_EXPOSURE_MIN,
					     SC1320CS_EXPOSURE_MAX, 1,
					     SC1320CS_EXPOSURE_DEFAULT);
	v4l2_ctrl_new_std(&sensor->ctrls, NULL, V4L2_CID_ANALOGUE_GAIN,
			  SC1320CS_ANALOGUE_GAIN_DEFAULT,
			  SC1320CS_ANALOGUE_GAIN_DEFAULT, 1,
			  SC1320CS_ANALOGUE_GAIN_DEFAULT);
	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		goto free_ctrls;

	ret = v4l2_ctrl_new_fwnode_properties(&sensor->ctrls,
					      &sc1320cs_ctrl_ops, &props);
	if (ret)
		goto free_ctrls;

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		goto free_ctrls;
	}

	link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	vblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->sd.ctrl_handler = &sensor->ctrls;
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return ret;
}

static void sc1320cs_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = SC1320CS_NATIVE_WIDTH;
	fmt->height = SC1320CS_NATIVE_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
}

static int sc1320cs_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sc1320cs_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = SC1320CS_NATIVE_WIDTH;
	fse->max_width = SC1320CS_NATIVE_WIDTH;
	fse->min_height = SC1320CS_NATIVE_HEIGHT;
	fse->max_height = SC1320CS_NATIVE_HEIGHT;
	return 0;
}

static int sc1320cs_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *fmt)
{
	if (fmt->pad)
		return -EINVAL;

	sc1320cs_fill_format(&fmt->format);
	return 0;
}

static int sc1320cs_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *fmt)
{
	if (fmt->pad)
		return -EINVAL;

	sc1320cs_fill_format(&fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;

	return 0;
}

static int sc1320cs_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_selection *sel)
{
	if (sel->pad)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = SC1320CS_NATIVE_WIDTH;
		sel->r.height = SC1320CS_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sc1320cs_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				    struct v4l2_mbus_config *config)
{
	unsigned int i;

	if (pad)
		return -EINVAL;

	config->type = V4L2_MBUS_CSI2_DPHY;
	config->link_freq = SC1320CS_LINK_FREQ;
	config->bus.mipi_csi2.flags = 0;
	config->bus.mipi_csi2.clock_lane = 7;
	config->bus.mipi_csi2.num_data_lanes = SC1320CS_NUM_DATA_LANES;
	for (i = 0; i < SC1320CS_NUM_DATA_LANES; i++)
		config->bus.mipi_csi2.data_lanes[i] = i;

	return 0;
}

static int sc1320cs_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct sc1320cs *sensor = to_sc1320cs(sd);
	int ret = 0;

	mutex_lock(&sensor->mutex);
	if (!enable) {
		if (sensor->streaming)
			ret = cci_write(sensor->regmap, CCI_REG8(0x0100), 0x00,
					NULL);
		sensor->streaming = false;
		sc1320cs_power_off(sensor);
		goto unlock;
	}

	if (sensor->streaming)
		goto unlock;

	ret = sc1320cs_power_on(sensor);
	if (ret)
		goto unlock;

	ret = sc1320cs_identify(sensor);
	if (ret)
		goto power_off;

	ret = sc1320cs_write_mode(sensor);
	if (ret)
		goto power_off;

	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (ret)
		goto power_off;

	ret = cci_write(sensor->regmap, CCI_REG8(0x0100), 0x01, NULL);
	if (ret)
		goto power_off;

	sensor->streaming = true;
	dev_info(sensor->dev, "SC1320CS streaming 4208x3120 RAW10\n");
	goto unlock;

power_off:
	sc1320cs_power_off(sensor);
unlock:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static const struct v4l2_subdev_video_ops sc1320cs_video_ops = {
	.s_stream = sc1320cs_set_stream,
};

static const struct v4l2_subdev_pad_ops sc1320cs_pad_ops = {
	.enum_mbus_code = sc1320cs_enum_mbus_code,
	.enum_frame_size = sc1320cs_enum_frame_size,
	.get_fmt = sc1320cs_get_fmt,
	.set_fmt = sc1320cs_set_fmt,
	.get_selection = sc1320cs_get_selection,
	.get_mbus_config = sc1320cs_get_mbus_config,
};

static const struct v4l2_subdev_ops sc1320cs_subdev_ops = {
	.video = &sc1320cs_video_ops,
	.pad = &sc1320cs_pad_ops,
};

static const struct media_entity_operations sc1320cs_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int sc1320cs_get_resources(struct sc1320cs *sensor)
{
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	unsigned long rate;
	unsigned int i;
	int ret;

	sensor->xvclk = devm_clk_get(sensor->dev, "xvclk");
	if (IS_ERR(sensor->xvclk))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->xvclk),
				     "failed to get MCLK1\n");

	rate = clk_get_rate(sensor->xvclk);
	if (rate != SC1320CS_XVCLK_FREQ)
		dev_warn(sensor->dev,
			 "xvclk is %lu Hz; Caihong downstream uses %u Hz\n",
			 rate, SC1320CS_XVCLK_FREQ);

	sensor->reset_gpio = devm_gpiod_get(sensor->dev, "reset",
					     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(sensor->dev,
				     PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO82\n");

	for (i = 0; i < ARRAY_SIZE(sc1320cs_supply_names); i++)
		sensor->supplies[i].supply = sc1320cs_supply_names[i];

	ret = devm_regulator_bulk_get(sensor->dev,
				       ARRAY_SIZE(sensor->supplies),
				       sensor->supplies);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "failed to get rear-camera supplies\n");

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(sensor->dev), NULL);
	if (!endpoint)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "missing CSI-2 endpoint\n");

	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "failed to parse CSI-2 endpoint\n");

	if (ep.bus.mipi_csi2.num_data_lanes != SC1320CS_NUM_DATA_LANES)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "expected %u CSI-2 lanes, DT has %u\n",
				     SC1320CS_NUM_DATA_LANES,
				     ep.bus.mipi_csi2.num_data_lanes);

	return 0;
}

static int sc1320cs_probe(struct i2c_client *client)
{
	struct sc1320cs *sensor;
	int ret;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &client->dev;
	mutex_init(&sensor->mutex);
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(sensor->regmap),
				     "failed to initialize CCI regmap\n");

	ret = sc1320cs_get_resources(sensor);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&sensor->sd, client, &sc1320cs_subdev_ops);
	ret = sc1320cs_power_on(sensor);
	if (ret)
		return ret;

	ret = sc1320cs_find_address(client, sensor);
	sc1320cs_power_off(sensor);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "SC1320CS was not found on CCI0 master 1\n");

	ret = sc1320cs_init_controls(sensor);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to initialize rear-camera controls\n");

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->sd.entity.ops = &sc1320cs_entity_ops;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret) {
		dev_err_probe(&client->dev, ret,
			      "failed to initialize rear-camera media entity\n");
		goto free_ctrls;
	}

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret) {
		media_entity_cleanup(&sensor->sd.entity);
		dev_err_probe(&client->dev, ret,
			      "failed to register rear-camera subdevice\n");
		goto free_ctrls;
	}

	dev_info(sensor->dev,
		 "V4L2 subdevice registered; streaming uses Caihong 4208x3120 mode\n");
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return ret;
}

static void sc1320cs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc1320cs *sensor = to_sc1320cs(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	media_entity_cleanup(&sd->entity);
	if (sensor->streaming)
		cci_write(sensor->regmap, CCI_REG8(0x0100), 0x00, NULL);
	sensor->streaming = false;
	sc1320cs_power_off(sensor);
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

MODULE_DESCRIPTION("SmartSens SC1320CS V4L2 sensor driver for Caihong");
MODULE_LICENSE("GPL");
