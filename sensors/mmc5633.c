// SPDX-License-Identifier: GPL-2.0-only
/*
 * MEMSIC MMC5603/MMC5633 I2C magnetometer external driver for Linux v7.2.
 *
 * Derived from drivers/iio/magnetometer/mmc5633.c in Linux v7.2
 * (NXP, 2025). Caihong exposes the mmc56x3x device on I2C, so the in-tree
 * I3C/HDR transport is intentionally omitted from this out-of-tree variant.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

#define MMC5633_REG_XOUT0	0x00
#define MMC5633_REG_XOUT1	0x01
#define MMC5633_REG_YOUT0	0x02
#define MMC5633_REG_YOUT1	0x03
#define MMC5633_REG_ZOUT0	0x04
#define MMC5633_REG_ZOUT1	0x05
#define MMC5633_REG_XOUT2	0x06
#define MMC5633_REG_YOUT2	0x07
#define MMC5633_REG_ZOUT2	0x08
#define MMC5633_REG_TOUT	0x09
#define MMC5633_REG_STATUS1	0x18
#define MMC5633_REG_CTRL0	0x1b
#define MMC5633_REG_CTRL1	0x1c
#define MMC5633_REG_ID		0x39

#define MMC5633_STATUS1_MEAS_T_DONE_BIT	BIT(7)
#define MMC5633_STATUS1_MEAS_M_DONE_BIT	BIT(6)
#define MMC5633_CTRL0_RESET			BIT(4)
#define MMC5633_CTRL0_SET			BIT(3)
#define MMC5633_CTRL0_MEAS_T			BIT(1)
#define MMC5633_CTRL0_MEAS_M			BIT(0)
#define MMC5633_CTRL1_BW_MASK			GENMASK(1, 0)
#define MMC5633_WAIT_SET_RESET_US		USEC_PER_MSEC

#define MMC5633_ALL_SIZE			10

enum mmc5633_axis {
	MMC5633_AXIS_X,
	MMC5633_AXIS_Y,
	MMC5633_AXIS_Z,
	MMC5633_TEMPERATURE,
};

struct mmc5633_data {
	struct regmap *regmap;
	struct mutex mutex;
};

static const int mmc5633_samp_freq[][2] = {
	{ 1, 200000 },
	{ 2, 0 },
	{ 3, 500000 },
	{ 6, 600000 },
};

#define MMC5633_CHANNEL(_axis) { \
	.type = IIO_MAGN, \
	.modified = 1, \
	.channel2 = IIO_MOD_ ## _axis, \
	.address = MMC5633_AXIS_ ## _axis, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ) | \
				    BIT(IIO_CHAN_INFO_SCALE), \
}

static const struct iio_chan_spec mmc5633_channels[] = {
	MMC5633_CHANNEL(X),
	MMC5633_CHANNEL(Y),
	MMC5633_CHANNEL(Z),
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.address = MMC5633_TEMPERATURE,
	},
};

static int mmc5633_get_samp_freq_index(int val, int val2)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mmc5633_samp_freq); i++)
		if (mmc5633_samp_freq[i][0] == val &&
		    mmc5633_samp_freq[i][1] == val2)
			return i;

	return -EINVAL;
}

static int mmc5633_init(struct mmc5633_data *data)
{
	unsigned int reg_id;
	int ret;

	ret = regmap_read(data->regmap, MMC5633_REG_ID, &reg_id);
	if (ret)
		return dev_err_probe(regmap_get_device(data->regmap), ret,
				     "error reading product id\n");

	ret = regmap_write(data->regmap, MMC5633_REG_CTRL0, MMC5633_CTRL0_SET);
	if (ret)
		return ret;

	fsleep(MMC5633_WAIT_SET_RESET_US);

	ret = regmap_write(data->regmap, MMC5633_REG_CTRL0, MMC5633_CTRL0_RESET);
	if (ret)
		return ret;

	fsleep(MMC5633_WAIT_SET_RESET_US);

	return regmap_update_bits(data->regmap, MMC5633_REG_CTRL1,
				  MMC5633_CTRL1_BW_MASK,
				  FIELD_PREP(MMC5633_CTRL1_BW_MASK, 0));
}

static int mmc5633_take_measurement(struct mmc5633_data *data, int address)
{
	unsigned int status, done;
	unsigned int command;
	int ret;

	if (address == MMC5633_TEMPERATURE) {
		command = MMC5633_CTRL0_MEAS_T;
		done = MMC5633_STATUS1_MEAS_T_DONE_BIT;
	} else {
		command = MMC5633_CTRL0_MEAS_M;
		done = MMC5633_STATUS1_MEAS_M_DONE_BIT;
	}

	ret = regmap_write(data->regmap, MMC5633_REG_CTRL0, command);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(data->regmap, MMC5633_REG_STATUS1,
				       status, status & done,
				       10 * USEC_PER_MSEC,
				       1000 * USEC_PER_MSEC);
	if (ret)
		dev_err(regmap_get_device(data->regmap), "data not ready\n");

	return ret;
}

static int mmc5633_read_measurement(struct mmc5633_data *data, int address,
				    u8 *buf)
{
	int ret;

	ret = mmc5633_take_measurement(data, address);
	if (ret)
		return ret;

	if (address == MMC5633_TEMPERATURE)
		return regmap_bulk_read(data->regmap, MMC5633_REG_TOUT,
					buf + MMC5633_ALL_SIZE - 1, 1);

	return regmap_bulk_read(data->regmap, MMC5633_REG_XOUT0,
				buf, MMC5633_ALL_SIZE);
}

static int mmc5633_get_raw(int index, const u8 *buf, int *val)
{
	if (index == MMC5633_TEMPERATURE) {
		*val = buf[MMC5633_ALL_SIZE - 1];
		return 0;
	}

	*val = get_unaligned_be16(buf + 2 * index) << 4;
	*val |= buf[index + 6] >> 4;

	return 0;
}

static int mmc5633_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct mmc5633_data *data = iio_priv(indio_dev);
	u8 buf[MMC5633_ALL_SIZE] = { 0 };
	unsigned int reg, index;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->mutex);
		ret = mmc5633_read_measurement(data, chan->address, buf);
		mutex_unlock(&data->mutex);
		if (ret)
			return ret;

		mmc5633_get_raw(chan->address, buf, val);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_MAGN) {
			*val = 0;
			*val2 = 62500;
		} else {
			*val = 0;
			*val2 = 800000000;
		}
		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_OFFSET:
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		*val = -75;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		mutex_lock(&data->mutex);
		ret = regmap_read(data->regmap, MMC5633_REG_CTRL1, &reg);
		mutex_unlock(&data->mutex);
		if (ret)
			return ret;

		index = FIELD_GET(MMC5633_CTRL1_BW_MASK, reg);
		if (index >= ARRAY_SIZE(mmc5633_samp_freq))
			return -EINVAL;

		*val = mmc5633_samp_freq[index][0];
		*val2 = mmc5633_samp_freq[index][1];
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static int mmc5633_write_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     int val, int val2, long mask)
{
	struct mmc5633_data *data = iio_priv(indio_dev);
	int index, ret;

	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;

	index = mmc5633_get_samp_freq_index(val, val2);
	if (index < 0)
		return index;

	mutex_lock(&data->mutex);
	ret = regmap_update_bits(data->regmap, MMC5633_REG_CTRL1,
				 MMC5633_CTRL1_BW_MASK,
				 FIELD_PREP(MMC5633_CTRL1_BW_MASK, index));
	mutex_unlock(&data->mutex);

	return ret;
}

static int mmc5633_read_avail(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;

	*vals = (const int *)mmc5633_samp_freq;
	*length = ARRAY_SIZE(mmc5633_samp_freq) * 2;
	*type = IIO_VAL_INT_PLUS_MICRO;

	return IIO_AVAIL_LIST;
}

static const struct iio_info mmc5633_info = {
	.read_raw = mmc5633_read_raw,
	.write_raw = mmc5633_write_raw,
	.read_avail = mmc5633_read_avail,
};

static bool mmc5633_writeable_reg(struct device *dev, unsigned int reg)
{
	return reg == MMC5633_REG_CTRL0 || reg == MMC5633_REG_CTRL1;
}

static bool mmc5633_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case MMC5633_REG_XOUT0:
	case MMC5633_REG_XOUT1:
	case MMC5633_REG_YOUT0:
	case MMC5633_REG_YOUT1:
	case MMC5633_REG_ZOUT0:
	case MMC5633_REG_ZOUT1:
	case MMC5633_REG_XOUT2:
	case MMC5633_REG_YOUT2:
	case MMC5633_REG_ZOUT2:
	case MMC5633_REG_TOUT:
	case MMC5633_REG_STATUS1:
	case MMC5633_REG_CTRL0:
	case MMC5633_REG_CTRL1:
	case MMC5633_REG_ID:
		return true;
	default:
		return false;
	}
}

static bool mmc5633_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg != MMC5633_REG_CTRL0 && reg != MMC5633_REG_CTRL1;
}

static const struct reg_default mmc5633_reg_defaults[] = {
	{ MMC5633_REG_CTRL0, 0x00 },
	{ MMC5633_REG_CTRL1, 0x00 },
};

static const struct regmap_config mmc5633_regmap_config = {
	.name = "mmc5633",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MMC5633_REG_ID,
	.cache_type = REGCACHE_MAPLE,
	.writeable_reg = mmc5633_writeable_reg,
	.readable_reg = mmc5633_readable_reg,
	.volatile_reg = mmc5633_volatile_reg,
	.reg_defaults = mmc5633_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(mmc5633_reg_defaults),
};

static int mmc5633_suspend(struct device *dev)
{
	struct regmap *regmap = dev_get_regmap(dev, NULL);

	regcache_cache_only(regmap, true);
	return 0;
}

static int mmc5633_resume(struct device *dev)
{
	struct regmap *regmap = dev_get_regmap(dev, NULL);
	int ret;

	regcache_cache_only(regmap, false);
	regcache_mark_dirty(regmap);
	ret = regcache_sync_region(regmap, MMC5633_REG_CTRL0, MMC5633_REG_CTRL1);
	if (ret)
		dev_err(dev, "failed to restore control registers: %d\n", ret);

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mmc5633_pm_ops, mmc5633_suspend, mmc5633_resume);

static int mmc5633_probe(struct i2c_client *client)
{
	struct mmc5633_data *data;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init_i2c(client, &mmc5633_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "regmap init failed\n");

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->regmap = regmap;
	mutex_init(&data->mutex);

	indio_dev->info = &mmc5633_info;
	indio_dev->name = client->name;
	indio_dev->channels = mmc5633_channels;
	indio_dev->num_channels = ARRAY_SIZE(mmc5633_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = mmc5633_init(data);
	if (ret)
		return dev_err_probe(&client->dev, ret, "chip init failed\n");

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id mmc5633_of_match[] = {
	{ .compatible = "memsic,mmc5603" },
	{ .compatible = "memsic,mmc5633" },
	{ }
};
MODULE_DEVICE_TABLE(of, mmc5633_of_match);

static const struct i2c_device_id mmc5633_i2c_ids[] = {
	{ "mmc5603" },
	{ "mmc5633" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mmc5633_i2c_ids);

static struct i2c_driver mmc5633_driver = {
	.driver = {
		.name = "mmc5633",
		.of_match_table = mmc5633_of_match,
		.pm = pm_sleep_ptr(&mmc5633_pm_ops),
	},
	.probe = mmc5633_probe,
	.id_table = mmc5633_i2c_ids,
};
module_i2c_driver(mmc5633_driver);

MODULE_AUTHOR("Frank Li <Frank.li@nxp.com>");
MODULE_DESCRIPTION("MEMSIC MMC5603/MMC5633 I2C magnetometer external driver");
MODULE_LICENSE("GPL");
