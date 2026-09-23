// SPDX-License-Identifier: GPL-2.0-only
/* Bounded CPS8601 identification, not a wireless charging driver. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>

#define PEN_OWNER 32785
#define PEN_SET_HBOOST 0x10007
#define PEN_ACK_MS 2500
#define PEN_NAME "caihong-pen-power"
#define PEN_I2C_PATH "/soc@0/geniqup@9c0000/i2c@98c000"
#define PEN_TLMM_PATH "/soc@0/pinctrl@f100000"

/* No boot-time activation: select a stage explicitly after reaching userspace. */
static unsigned int stage;
module_param(stage, uint, 0400);
MODULE_PARM_DESC(stage, "Manual diagnostic: 1=transport only, 2=GPIO/I2C plus explicit probe_once");

/* Board-local lookup for the unmodified Stage6b DT; never use global GPIO IDs. */
static struct gpiod_lookup_table pen_gpios = {
	.dev_id = PEN_NAME,
	.table = {
		GPIO_LOOKUP("f100000.pinctrl", 111, "charge-disable", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("f100000.pinctrl", 10, "supply", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("f100000.pinctrl", 15, "wake", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("f100000.pinctrl", 85, "scan", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("f100000.pinctrl", 12, "irq", GPIO_ACTIVE_HIGH),
		{ }
	},
};

/* TLMM exposes bias via pinctrl, not gpio_chip.set_config on this kernel. */
static unsigned long pen_irq_config[] = {
	PIN_CONF_PACKED(PIN_CONFIG_BIAS_PULL_UP, 1),
};

static const struct pinctrl_map pen_pinmaps[] = {
	PIN_MAP_CONFIGS_GROUP(PEN_NAME, "irq-pull-up", "f100000.pinctrl",
			      "gpio12", pen_irq_config),
};

struct pen_boost_request {
	struct pmic_glink_hdr hdr;
	u8 value;
	u8 reserved[3];
};

struct pen_power {
	struct device *dev;
	struct pmic_glink_client *glink;
	struct i2c_client *i2c;
	struct gpio_desc *disable, *supply, *wake, *scan, *irq;
	struct mutex lock;
	spinlock_t ack_lock;
	struct completion ack, lost;
	bool up, pending, poisoned, active, suspended;
	bool hardware_ready;
	int ack_error;
	u32 rejected;
	int result, cleanup;
	const char *phase;
	u32 valid;
	u16 values[8];
};

static struct platform_device *pen_device;
static int pen_probe_result = -ENODEV;
/* One attempt per module load; unbinding/rebinding must not reset this. */
static bool pen_used;

static bool pen_reply_valid(const void *data, size_t len)
{
	const u8 *bytes = data;

	return len == 16 && get_unaligned_le32(bytes) == PEN_OWNER &&
		get_unaligned_le32(bytes + 4) == PMIC_GLINK_REQ_RESP &&
		get_unaligned_le32(bytes + 8) == PEN_SET_HBOOST;
}

static void pen_reply(const void *data, size_t len, void *priv)
{
	struct pen_power *pen = priv;
	unsigned long flags;

	spin_lock_irqsave(&pen->ack_lock, flags);
	if (!pen_reply_valid(data, len) || !pen->pending) {
		pen->rejected++;
	} else {
		pen->ack_error = get_unaligned_le32((const u8 *)data + 12) ?
			-EREMOTEIO : 0;
		pen->pending = false;
		complete(&pen->ack);
	}
	spin_unlock_irqrestore(&pen->ack_lock, flags);
}

static void pen_transport(void *priv, int state)
{
	struct pen_power *pen = priv;
	unsigned long flags;

	/* PMIC-Glink invokes callbacks under a spinlock; never sleep here. */
	spin_lock_irqsave(&pen->ack_lock, flags);
	pen->up = state == SERVREG_SERVICE_STATE_UP;
	if (!pen->up) {
		if (pen->active)
			pen->poisoned = true;
		if (pen->pending) {
			pen->ack_error = -ENOTCONN;
			pen->pending = false;
			complete(&pen->ack);
		}
		complete_all(&pen->lost);
	}
	spin_unlock_irqrestore(&pen->ack_lock, flags);
}

static int pen_set_boost(struct pen_power *pen, u8 value)
{
	struct pen_boost_request req = {
		.hdr = {
			.owner = cpu_to_le32(PEN_OWNER),
			.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
			.opcode = cpu_to_le32(PEN_SET_HBOOST),
		},
		.value = value,
	};
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pen->ack_lock, flags);
	if (!pen->up || pen->poisoned) {
		ret = pen->poisoned ? -EPIPE : -ENOTCONN;
		spin_unlock_irqrestore(&pen->ack_lock, flags);
		return ret;
	}
	reinit_completion(&pen->ack);
	pen->pending = true;
	pen->ack_error = -EINPROGRESS;
	spin_unlock_irqrestore(&pen->ack_lock, flags);

	ret = pmic_glink_send(pen->glink, &req, sizeof(req));
	if (!ret)
		wait_for_completion_timeout(&pen->ack, msecs_to_jiffies(PEN_ACK_MS));

	spin_lock_irqsave(&pen->ack_lock, flags);
	if (ret || pen->pending) {
		/* No request cookie: a late ACK cannot identify a subsequent request. */
		pen->poisoned = true;
		pen->pending = false;
		pen->ack_error = ret ?: -ETIMEDOUT;
	}
	ret = pen->ack_error;
	if (!ret && pen->poisoned)
		ret = -ENOTCONN;
	spin_unlock_irqrestore(&pen->ack_lock, flags);
	return ret;
}

static void pen_off(struct pen_power *pen)
{
	gpiod_set_value_cansleep(pen->disable, 1);
	gpiod_set_value_cansleep(pen->wake, 0);
	gpiod_set_value_cansleep(pen->scan, 0);
	gpiod_set_value_cansleep(pen->supply, 0);
}

static int pen_delay(struct pen_power *pen, unsigned int ms)
{
	return wait_for_completion_timeout(&pen->lost, msecs_to_jiffies(ms)) ?
		-ENOTCONN : 0;
}

static int pen_read(struct pen_power *pen, u16 reg, unsigned int len, u16 *value)
{
	u16 result = 0;
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		u8 selector[2], byte;
		struct i2c_msg messages[] = {
			{ .addr = 0x41, .len = 2, .buf = selector },
			{ .addr = 0x41, .flags = I2C_M_RD, .len = 1, .buf = &byte },
		};

		if (completion_done(&pen->lost))
			return -ENOTCONN;
		put_unaligned_be16(reg + i, selector);
		ret = i2c_transfer(pen->i2c->adapter, messages, ARRAY_SIZE(messages));
		if (ret != ARRAY_SIZE(messages))
			return ret < 0 ? ret : -EIO;
		result |= (u16)byte << (8 * i);
	}
	*value = result;
	return 0;
}

static int pen_identify(struct pen_power *pen)
{
	static const u16 registers[] = { 0, 2, 4, 7, 0x34, 0x38, 0x3a, 0x3e };
	static const u8 sizes[] = { 2, 2, 1, 2, 2, 2, 1, 2 };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = pen_read(pen, registers[i], sizes[i], &pen->values[i]);
		if (ret)
			return ret;
		pen->valid |= BIT(i);
		if (!i && pen->values[0] != 0x8601)
			return -ENODEV;
	}
	return 0;
}

static int pen_run(struct pen_power *pen)
{
	int ret;

	if (!pen->hardware_ready)
		return -EOPNOTSUPP;
	pen_off(pen);
	pen->phase = "off-check";
	if (gpiod_get_value_cansleep(pen->disable) != 1 ||
	    gpiod_get_value_cansleep(pen->supply) != 0)
		return -EIO;
	pen->phase = "hboost";
	dev_info(pen->dev, "probe=hboost begin\n");
	ret = pen_set_boost(pen, 76); /* (5800 mV - 2000 mV) / 50 */
	if (ret)
		goto out;
	pen->phase = "supply";
	dev_info(pen->dev, "probe=supply begin\n");
	gpiod_set_value_cansleep(pen->supply, 1);
	if (gpiod_get_value_cansleep(pen->supply) != 1) {
		ret = -EIO;
		goto out;
	}
	ret = pen_delay(pen, 10);
	if (ret)
		goto out;
	pen->phase = "wake";
	dev_info(pen->dev, "probe=wake begin\n");
	gpiod_set_value_cansleep(pen->wake, 1);
	if (gpiod_get_value_cansleep(pen->wake) != 1) {
		ret = -EIO;
		goto out;
	}
	ret = pen_delay(pen, 2500);
	if (!ret) {
		pen->phase = "read-id-status";
		dev_info(pen->dev, "probe=read-id-status begin\n");
		ret = pen_identify(pen);
	}
out:
	/* Cut the physical path before attempting the minimum HBOOST request. */
	dev_info(pen->dev, "probe=power-off begin; result=%d\n", ret);
	pen_off(pen);
	pen->cleanup = pen_set_boost(pen, 0);
	if (gpiod_get_value_cansleep(pen->disable) != 1 ||
	    gpiod_get_value_cansleep(pen->wake) != 0 ||
	    gpiod_get_value_cansleep(pen->supply) != 0)
		pen->cleanup = -EIO;
	if (!ret)
		pen->phase = pen->cleanup ? "cleanup" : "done";
	dev_info(pen->dev, "probe=power-off complete; cleanup=%d\n", pen->cleanup);
	return ret ?: pen->cleanup;
}

static ssize_t probe_once_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct pen_power *pen = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	if (!pen->hardware_ready)
		return -EOPNOTSUPP;
	if (!mutex_trylock(&pen->lock))
		return -EBUSY;
	if (pen_used || pen->suspended) {
		ret = pen_used ? -EALREADY : -EBUSY;
		goto unlock;
	}
	spin_lock_irqsave(&pen->ack_lock, flags);
	if (!pen->up) {
		spin_unlock_irqrestore(&pen->ack_lock, flags);
		ret = -ENOTCONN;
		goto unlock;
	}
	reinit_completion(&pen->lost);
	pen->active = true;
	spin_unlock_irqrestore(&pen->ack_lock, flags);
	pen_used = true;
	pm_stay_awake(dev);
	pen->result = pen_run(pen);
	spin_lock_irqsave(&pen->ack_lock, flags);
	pen->active = false;
	spin_unlock_irqrestore(&pen->ack_lock, flags);
	pm_relax(dev);
	dev_info(dev, "ID diagnostic result=%d cleanup=%d valid=%#x chip_id=%#x\n",
		 pen->result, pen->cleanup, pen->valid, pen->values[0]);
	ret = pen->result;
unlock:
	mutex_unlock(&pen->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(probe_once);

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pen_power *pen = dev_get_drvdata(dev);
	unsigned long flags;
	bool up, poisoned;
	u32 rejected;
	ssize_t count;

	mutex_lock(&pen->lock);
	spin_lock_irqsave(&pen->ack_lock, flags);
	up = pen->up;
	poisoned = pen->poisoned;
	rejected = pen->rejected;
	spin_unlock_irqrestore(&pen->ack_lock, flags);
	count = sysfs_emit(buf,
		"stage=%u hardware_ready=%u attempted=%u transport_up=%u poisoned=%u rejected=%u phase=%s result=%d cleanup=%d valid=%#x\n"
		"chip_id=%#06x firmware=%#06x mode=%#x irq=%#x vin_raw=%u iin_raw=%u temperature_raw=%u ept=%#x\n"
		"charge_disable=%d supply=%d wake=%d scan=%d irq_level=%d\n",
		stage, pen->hardware_ready, pen_used, up, poisoned, rejected,
		pen->phase, pen->result, pen->cleanup, pen->valid,
		pen->values[0], pen->values[1], pen->values[2], pen->values[3],
		pen->values[4], pen->values[5], pen->values[6], pen->values[7],
		pen->hardware_ready ? gpiod_get_value_cansleep(pen->disable) : -ENODEV,
		pen->hardware_ready ? gpiod_get_value_cansleep(pen->supply) : -ENODEV,
		pen->hardware_ready ? gpiod_get_value_cansleep(pen->wake) : -ENODEV,
		pen->hardware_ready ? gpiod_get_value_cansleep(pen->scan) : -ENODEV,
		pen->hardware_ready ? gpiod_get_value_cansleep(pen->irq) : -ENODEV);
	mutex_unlock(&pen->lock);
	return count;
}
static DEVICE_ATTR_RO(status);

static struct attribute *pen_attrs[] = {
	&dev_attr_probe_once.attr,
	&dev_attr_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pen);

static void pen_put_adapter(void *data)
{
	i2c_put_adapter(data);
}

static int pen_get_hardware(struct pen_power *pen)
{
	struct device *dev = pen->dev;
	struct platform_device *tlmm;
	struct device_node *bus;
	struct i2c_adapter *adapter;
	struct pinctrl *pinctrl;
	struct pinctrl_state *irq_state;
	int ret;

	dev_info(dev, "setup=i2c begin\n");
	bus = of_find_node_by_path(PEN_I2C_PATH);
	if (!bus)
		return -ENODEV;
	adapter = i2c_get_adapter_by_fwnode(of_fwnode_handle(bus));
	of_node_put(bus);
	if (!adapter)
		return -ENODEV;
	ret = devm_add_action_or_reset(dev, pen_put_adapter, adapter);
	if (ret)
		return ret;
	if (!device_link_add(dev, adapter->dev.parent, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	if (!device_is_bound(adapter->dev.parent))
		return -ENODEV;
	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	pen->i2c = devm_i2c_new_dummy_device(dev, adapter, 0x41);
	if (IS_ERR(pen->i2c))
		return dev_err_probe(dev, PTR_ERR(pen->i2c), "reserve CPS8601 address\n");
	dev_info(dev, "setup=i2c complete; address reserved, no transfer\n");

	bus = of_find_node_by_path(PEN_TLMM_PATH);
	if (!bus)
		return -ENODEV;
	tlmm = of_find_device_by_node(bus);
	of_node_put(bus);
	if (!tlmm)
		return -ENODEV;
	ret = -ENODEV;
	if (of_device_is_compatible(tlmm->dev.of_node, "qcom,sm8650-tlmm") &&
	    !strcmp(dev_name(&tlmm->dev), "f100000.pinctrl") &&
	    device_link_add(dev, &tlmm->dev, DL_FLAG_AUTOREMOVE_CONSUMER) &&
	    device_is_bound(&tlmm->dev))
		ret = 0;
	put_device(&tlmm->dev);
	if (ret)
		return ret;

	/* Acquire the charge-inhibit line before any power/wake output. */
	dev_info(dev, "setup=gpio111 inhibit begin\n");
	pen->disable = devm_gpiod_get(dev, "charge-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(pen->disable))
		return dev_err_probe(dev, PTR_ERR(pen->disable), "charge-disable GPIO\n");
	dev_info(dev, "setup=gpio10 supply-off begin\n");
	pen->supply = devm_gpiod_get(dev, "supply", GPIOD_OUT_LOW);
	if (IS_ERR(pen->supply))
		return dev_err_probe(dev, PTR_ERR(pen->supply), "supply GPIO\n");
	dev_info(dev, "setup=gpio15 wake-low begin\n");
	pen->wake = devm_gpiod_get(dev, "wake", GPIOD_OUT_LOW);
	if (IS_ERR(pen->wake))
		return dev_err_probe(dev, PTR_ERR(pen->wake), "wake GPIO\n");
	dev_info(dev, "setup=gpio85 scan-low begin\n");
	pen->scan = devm_gpiod_get(dev, "scan", GPIOD_OUT_LOW);
	if (IS_ERR(pen->scan))
		return dev_err_probe(dev, PTR_ERR(pen->scan), "scan GPIO\n");
	dev_info(dev, "setup=gpio12 irq-input begin\n");
	pen->irq = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(pen->irq))
		return dev_err_probe(dev, PTR_ERR(pen->irq), "IRQ GPIO\n");
	dev_info(dev, "setup=irq-pinctrl begin\n");
	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(dev, PTR_ERR(pinctrl), "IRQ pinctrl\n");
	irq_state = pinctrl_lookup_state(pinctrl, "irq-pull-up");
	if (IS_ERR(irq_state))
		return dev_err_probe(dev, PTR_ERR(irq_state), "IRQ bias state\n");
	ret = pinctrl_select_state(pinctrl, irq_state);
	if (ret)
		return dev_err_probe(dev, ret, "IRQ pull-up\n");
	pen->hardware_ready = true;
	dev_info(dev, "setup=gpio complete; supply off, charging inhibited\n");
	return 0;
}

static int pen_setup(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pen_power *pen;
	int ret;

	if (!dev->parent || !device_is_bound(dev->parent) ||
	    !of_device_is_compatible(dev->parent->of_node, "qcom,pmic-glink"))
		return -ENODEV;
	pen = devm_kzalloc(dev, sizeof(*pen), GFP_KERNEL);
	if (!pen)
		return -ENOMEM;
	pen->dev = dev;
	pen->result = pen->cleanup = -ENODATA;
	pen->phase = "idle";
	mutex_init(&pen->lock);
	spin_lock_init(&pen->ack_lock);
	init_completion(&pen->ack);
	init_completion(&pen->lost);
	platform_set_drvdata(pdev, pen);

	dev_info(dev, "setup=transport begin\n");
	pen->glink = devm_pmic_glink_client_alloc(dev, PEN_OWNER, pen_reply, pen_transport, pen);
	if (IS_ERR(pen->glink))
		return PTR_ERR(pen->glink);
	pmic_glink_client_register(pen->glink);
	dev_info(dev, "setup=transport complete; no request sent\n");
	if (stage == 2) {
		ret = pen_get_hardware(pen);
		if (ret)
			return ret;
	}
	ret = device_init_wakeup(dev, true);
	if (ret)
		return ret;
	ret = sysfs_create_groups(&dev->kobj, pen_groups);
	if (ret) {
		device_init_wakeup(dev, false);
		return ret;
	}
	dev_info(dev, "setup=ready stage=%u hardware_ready=%u; waiting for userspace\n",
		 stage, pen->hardware_ready);
	return 0;
}

static int pen_probe(struct platform_device *pdev)
{
	pen_probe_result = pen_setup(pdev);
	if (pen_probe_result)
		dev_err(&pdev->dev, "setup failed: %d\n", pen_probe_result);
	return pen_probe_result;
}

static void pen_stop(struct platform_device *pdev)
{
	struct pen_power *pen = platform_get_drvdata(pdev);

	mutex_lock(&pen->lock);
	pen->suspended = true;
	if (pen->hardware_ready)
		pen_off(pen);
	mutex_unlock(&pen->lock);
}

static void pen_remove(struct platform_device *pdev)
{
	sysfs_remove_groups(&pdev->dev.kobj, pen_groups);
	pen_stop(pdev);
	device_init_wakeup(&pdev->dev, false);
}

static int pen_suspend(struct device *dev)
{
	struct pen_power *pen = dev_get_drvdata(dev);

	if (!mutex_trylock(&pen->lock))
		return -EBUSY;
	pen->suspended = true;
	if (pen->hardware_ready)
		pen_off(pen);
	mutex_unlock(&pen->lock);
	return 0;
}

static int pen_resume(struct device *dev)
{
	struct pen_power *pen = dev_get_drvdata(dev);

	mutex_lock(&pen->lock);
	if (pen->hardware_ready)
		pen_off(pen);
	pen->suspended = false;
	mutex_unlock(&pen->lock);
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(pen_pm, pen_suspend, pen_resume);

static struct platform_driver pen_driver = {
	.probe = pen_probe,
	.remove = pen_remove,
	.shutdown = pen_stop,
	.prevent_deferred_probe = true,
	.driver = {
		.name = PEN_NAME,
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&pen_pm),
	},
};

static int __init pen_init(void)
{
	struct device_node *np;
	struct platform_device *parent;
	int ret = -ENODEV;

	if (stage != 1 && stage != 2) {
		pr_err("manual post-boot diagnostic only; specify stage=1 or stage=2\n");
		return -EINVAL;
	}
	if (!of_machine_is_compatible("oneplus,caihong"))
		return ret;
	np = of_find_node_by_path("/pmic-glink");
	if (!np)
		return ret;
	parent = of_find_device_by_node(np);
	if (!parent)
		goto put_node;
	/* A real child plus supplier link, with no DT/boot image changes. */
	pr_info("setup=parent-lock begin stage=%u\n", stage);
	if (!device_trylock(&parent->dev)) {
		ret = -EBUSY;
		goto put_parent;
	}
	if (!device_is_bound(&parent->dev) ||
	    !of_device_is_compatible(parent->dev.of_node, "qcom,pmic-glink"))
		goto unlock_parent;
	pr_info("setup=child-add begin\n");
	pen_device = platform_device_alloc(PEN_NAME, PLATFORM_DEVID_NONE);
	if (!pen_device) {
		ret = -ENOMEM;
		goto unlock_parent;
	}
	pen_device->dev.parent = &parent->dev;
	ret = platform_device_add(pen_device);
	if (ret) {
		platform_device_put(pen_device);
		pen_device = NULL;
		goto unlock_parent;
	}
	pr_info("setup=provider-link begin\n");
	if (!device_link_add(&pen_device->dev, &parent->dev, DL_FLAG_AUTOPROBE_CONSUMER)) {
		ret = -EINVAL;
		goto unlock_parent;
	}
	ret = 0;
unlock_parent:
	device_unlock(&parent->dev);
put_parent:
	put_device(&parent->dev);
put_node:
	of_node_put(np);
	if (ret) {
		if (pen_device)
			platform_device_unregister(pen_device);
		return ret;
	}
	if (stage == 2) {
		ret = pinctrl_register_mappings(pen_pinmaps, ARRAY_SIZE(pen_pinmaps));
		if (ret)
			goto unregister_device;
		gpiod_add_lookup_table(&pen_gpios);
	}
	pr_info("setup=driver-register begin\n");
	ret = platform_driver_register(&pen_driver);
	if (!ret && pen_probe_result) {
		ret = pen_probe_result;
		platform_driver_unregister(&pen_driver);
	}
	if (ret) {
		if (stage == 2) {
			gpiod_remove_lookup_table(&pen_gpios);
			pinctrl_unregister_mappings(pen_pinmaps);
		}
		goto unregister_device;
	}
	return 0;

unregister_device:
	platform_device_unregister(pen_device);
	return ret;
}
module_init(pen_init);

static void __exit pen_exit(void)
{
	platform_device_unregister(pen_device);
	platform_driver_unregister(&pen_driver);
	if (stage == 2) {
		gpiod_remove_lookup_table(&pen_gpios);
		pinctrl_unregister_mappings(pen_pinmaps);
	}
}
module_exit(pen_exit);

MODULE_DESCRIPTION("Caihong CPS8601 manual staged transport and chip ID diagnostic");
MODULE_VERSION("7.1");
MODULE_LICENSE("GPL");
