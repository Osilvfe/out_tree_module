// SPDX-License-Identifier: GPL-2.0-only
/*
 * ROHM BU52053NVX Hall switch input driver
 *
 * Caihong's Qualcomm SSC registry describes this device as a GPIO-only Hall
 * sensor on SoC TLMM GPIO66 with dual-edge interrupts.  The Linux driver keeps
 * the interface intentionally standard: EV_SW/SW_LID via the input subsystem.
 *
 * The downstream registry names the supply only as the SSC PMIC client
 * "/pmic/client/sensor_vddio", which does not identify an AP Linux regulator
 * phandle.  Therefore vddio-supply is optional: if present Linux manages it;
 * otherwise the driver assumes the board/firmware keeps the rail powered.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

struct bu52053nvx {
	struct device *dev;
	struct gpio_desc *hall_gpio;
	struct regulator *vddio;
	struct input_dev *input;
	unsigned int code;
	int irq;
};

static int bu52053nvx_read_active(struct bu52053nvx *hall)
{
	int value;

	/*
	 * gpiod_get_value_cansleep() returns the logical value and therefore
	 * already accounts for GPIO_ACTIVE_LOW from firmware description.
	 */
	value = gpiod_get_value_cansleep(hall->hall_gpio);
	if (value < 0)
		return value;

	return !!value;
}

static int bu52053nvx_report(struct bu52053nvx *hall)
{
	int active;

	active = bu52053nvx_read_active(hall);
	if (active < 0)
		return active;

	input_report_switch(hall->input, hall->code, active);
	input_sync(hall->input);

	return 0;
}

static irqreturn_t bu52053nvx_irq_thread(int irq, void *data)
{
	struct bu52053nvx *hall = data;
	int ret;

	ret = bu52053nvx_report(hall);
	if (ret)
		dev_err_ratelimited(hall->dev,
				    "failed to sample Hall GPIO: %d\n", ret);

	return IRQ_HANDLED;
}

static void bu52053nvx_disable_supply(void *data)
{
	struct regulator *vddio = data;

	regulator_disable(vddio);
}

static int bu52053nvx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bu52053nvx *hall;
	u32 code = SW_LID;
	int ret;

	hall = devm_kzalloc(dev, sizeof(*hall), GFP_KERNEL);
	if (!hall)
		return -ENOMEM;

	hall->dev = dev;

	hall->vddio = devm_regulator_get_optional(dev, "vddio");
	if (IS_ERR(hall->vddio)) {
		ret = PTR_ERR(hall->vddio);
		if (ret == -ENODEV || ret == -ENOENT) {
			hall->vddio = NULL;
		} else {
			return dev_err_probe(dev, ret,
					     "failed to get vddio supply\n");
		}
	}

	if (hall->vddio) {
		ret = regulator_enable(hall->vddio);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to enable vddio\n");

		ret = devm_add_action_or_reset(dev, bu52053nvx_disable_supply,
					       hall->vddio);
		if (ret)
			return ret;

		/* Allow the Hall IC output to settle after rail enable. */
		usleep_range(1000, 2000);
	}

	hall->hall_gpio = devm_gpiod_get(dev, "hall", GPIOD_IN);
	if (IS_ERR(hall->hall_gpio))
		return dev_err_probe(dev, PTR_ERR(hall->hall_gpio),
				     "failed to get Hall GPIO\n");

	device_property_read_u32(dev, "linux,code", &code);
	if (code > SW_MAX)
		return dev_err_probe(dev, -EINVAL,
				     "invalid switch code %u\n", code);
	hall->code = code;

	hall->input = devm_input_allocate_device(dev);
	if (!hall->input)
		return -ENOMEM;

	hall->input->name = "BU52053NVX Hall Switch";
	hall->input->phys = "bu52053nvx/input0";
	hall->input->id.bustype = BUS_HOST;
	hall->input->dev.parent = dev;
	input_set_capability(hall->input, EV_SW, hall->code);

	hall->irq = gpiod_to_irq(hall->hall_gpio);
	if (hall->irq < 0)
		return dev_err_probe(dev, hall->irq,
				     "failed to map Hall GPIO IRQ\n");

	/* Downstream SSC configuration uses trigger type 2: dual edge. */
	ret = devm_request_threaded_irq(dev, hall->irq, NULL,
					bu52053nvx_irq_thread,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT,
					dev_name(dev), hall);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request Hall IRQ\n");

	ret = devm_input_register_device(dev, hall->input);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register input device\n");

	platform_set_drvdata(pdev, hall);

	if (device_property_read_bool(dev, "wakeup-source")) {
		device_init_wakeup(dev, true);
		ret = dev_pm_set_wake_irq(dev, hall->irq);
		if (ret)
			dev_warn(dev, "failed to configure wake IRQ: %d\n", ret);
	}

	ret = bu52053nvx_report(hall);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed initial Hall state read\n");

	dev_info(dev, "BU52053NVX Hall switch registered on IRQ %d\n",
		 hall->irq);

	return 0;
}

static void bu52053nvx_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
}

static const struct of_device_id bu52053nvx_of_match[] = {
	{ .compatible = "rohm,bu52053nvx" },
	{ }
};
MODULE_DEVICE_TABLE(of, bu52053nvx_of_match);

static struct platform_driver bu52053nvx_driver = {
	.probe = bu52053nvx_probe,
	.remove = bu52053nvx_remove,
	.driver = {
		.name = "bu52053nvx-hall",
		.of_match_table = bu52053nvx_of_match,
	},
};
module_platform_driver(bu52053nvx_driver);

MODULE_DESCRIPTION("ROHM BU52053NVX GPIO Hall switch input driver");
MODULE_LICENSE("GPL");
