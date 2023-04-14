// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 NVIDIA Corporation
 * Copyright (C) 2023 Svyatoslav Ryhel <clamor95@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/devm-helpers.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regulator/consumer.h>
#include <linux/platform_device.h>
#include <linux/usb/chipidea.h>
#include <linux/usb/tegra_usb_phy.h>

#define MODEM_ENUM_TIMEOUT_200MS	25

enum ipc_ap_wake_state {
	IPC_AP_WAKE_IRQ_READY,
	IPC_AP_WAKE_INIT1,
	IPC_AP_WAKE_INIT2,
	IPC_AP_WAKE_L,
	IPC_AP_WAKE_H,
	IPC_AP_WAKE_UNINIT,
};

struct baseband_xmm_power_data {
	struct device *dev;

	struct platform_device *usb_dev;
	struct usb_phy *usb_phy;

	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;

	struct gpio_desc *ipc_cp_gpio;
	struct gpio_desc *ipc_ap_gpio;

	struct gpio_desc *hsic_en_gpio;

	struct regulator *vbus_supply;

	struct delayed_work modem_init_work;
	struct delayed_work modem_poll_work;

	int irq_hostwake;
	int prev_hostwake;
	enum ipc_ap_wake_state ap_state;

	bool powered;
};

static void baseband_xmm_init_work(struct work_struct *work)
{
	struct baseband_xmm_power_data *priv =
		container_of(work, struct baseband_xmm_power_data, modem_init_work.work);
	struct tegra_usb *usb = platform_get_drvdata(priv->usb_dev);

	if (!priv->powered) {
		device_lock(priv->dev);

		usb_phy_init(priv->usb_phy);
		gpiod_set_value_cansleep(priv->hsic_en_gpio, 1);
		usb->dev = ci_hdrc_add_device(&priv->usb_dev->dev, priv->usb_dev->resource,
					      priv->usb_dev->num_resources, &usb->data);
		priv->powered = true;

		device_unlock(priv->dev);
	}
};

static void baseband_xmm_poll_work(struct work_struct *work)
{
	struct baseband_xmm_power_data *priv =
		container_of(work, struct baseband_xmm_power_data, modem_poll_work.work);
	struct tegra_usb *usb = platform_get_drvdata(priv->usb_dev);
	int timeout_200ms = 0;

	/* waiting ttyACM dev to be created */
	do {
		struct file *filp;
		filp = filp_open("/dev/ttyACM0", O_RDONLY, 0);
		if (filp && !IS_ERR(filp)) {
			dev_dbg(priv->dev, "ttyACM0 created OK\n");
			filp_close(filp, NULL);
			return;
		}

		msleep(200);
	} while (++timeout_200ms <= MODEM_ENUM_TIMEOUT_200MS);

	dev_err(priv->dev, "modem registration failed\n");
	priv->ap_state = IPC_AP_WAKE_IRQ_READY;

	/* unregister usb host controller */
	dev_err(priv->dev, "deregistring USB\n");

	device_lock(priv->dev);

	ci_hdrc_remove_device(usb->dev);
	usb_phy_shutdown(priv->usb_phy);
	gpiod_set_value_cansleep(priv->hsic_en_gpio, 0);
	priv->powered = false;

	device_unlock(priv->dev);

	msleep(500);
}

static irqreturn_t baseband_hostwake_interrupt(int irq, void *dev_id)
{
	struct baseband_xmm_power_data *priv = dev_id;
	int state = gpiod_get_value(priv->ipc_ap_gpio);

	switch (priv->ap_state) {
	case IPC_AP_WAKE_IRQ_READY:
		if (!state) {
			priv->ap_state = IPC_AP_WAKE_INIT1;
			schedule_delayed_work(&priv->modem_init_work, 0);
		}

		dev_dbg(priv->dev, "hostwake: %d, ap_state: %d\n", state, priv->ap_state);
		break;

	case IPC_AP_WAKE_INIT1:
		if (state) {
			priv->ap_state = IPC_AP_WAKE_INIT2;
			schedule_delayed_work(&priv->modem_poll_work, 0);
		}

		dev_dbg(priv->dev, "hostwake: %d, ap_state: %d\n", state, priv->ap_state);
		break;

	default:
//		if (state)
//			priv->ap_state = IPC_AP_WAKE_H;
//		else
//			priv->ap_state = IPC_AP_WAKE_L;
//
//		dev_err(priv->dev, "hostwake: %d, ap_state: %d\n", state, priv->ap_state);
		break;
	}

	return IRQ_HANDLED;
}

#if 0
static ssize_t ehci_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct baseband_xmm_power_data *priv = platform_get_drvdata(pdev);

	return sysfs_emit(buf, "%u\n", priv->powered);
}

static ssize_t ehci_power_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct baseband_xmm_power_data *priv = platform_get_drvdata(pdev);
	struct tegra_usb *usb = platform_get_drvdata(priv->usb_dev);
	unsigned long power_on;
	int ret;

	ret = kstrtoul(buf, 10, &power_on);
	if (ret)
		return ret;

	if (power_on != priv->powered) {
		device_lock(dev);
		if (power_on) {
			dev_info(dev, "Powering on EHCI\n");

			usb_phy_init(priv->usb_phy);
			usb->dev = ci_hdrc_add_device(&priv->usb_dev->dev, priv->usb_dev->resource,
						      priv->usb_dev->num_resources, &usb->data);
		} else {
			dev_info(dev, "Powering off EHCI\n");

			ci_hdrc_remove_device(usb->dev);
			usb_phy_shutdown(priv->usb_phy);
		}
		priv->powered = power_on;
		device_unlock(dev);
	}

	return count;
}
static DEVICE_ATTR_RW(ehci_power);
#endif

static void baseband_xmm_reset(struct baseband_xmm_power_data *priv)
{
	int ret;

	ret = regulator_enable(priv->vbus_supply);
	if (ret < 0)
		dev_err(priv->dev, "failed to enable vbat power supply\n");

	/* reset / power on sequence */
	gpiod_set_value_cansleep(priv->enable_gpio, 0);
	msleep(50);

	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	msleep(200);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);

	msleep(50);
	
	gpiod_set_value_cansleep(priv->enable_gpio, 1);
	udelay(60);
	gpiod_set_value_cansleep(priv->enable_gpio, 0);
	msleep(20);
//	gpiod_set_value_cansleep(priv->enable_gpio, 1);
//	udelay(60);
}

static void baseband_xmm_power_off(void *data)
{
	struct baseband_xmm_power_data *priv = data;

	gpiod_set_value_cansleep(priv->enable_gpio, 0);
	gpiod_set_value_cansleep(priv->reset_gpio, 1);

	regulator_disable(priv->vbus_supply);
}

static int baseband_xmm_power_probe(struct platform_device *pdev)
{
	struct baseband_xmm_power_data *priv;
	struct device *dev = &pdev->dev;
	struct device_node *usb_node;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	priv->vbus_supply = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(priv->vbus_supply))
		return dev_err_probe(dev, PTR_ERR(priv->vbus_supply),
				     "failed to get vbus regulator\n");

	/* Own modem gpios */
	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "failed to get reset GPIO\n");

	priv->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(priv->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->enable_gpio),
				     "failed to get enable GPIO\n");

	/* CP - AP connections */
	priv->ipc_cp_gpio = devm_gpiod_get_optional(dev, "link-slavewake", GPIOD_OUT_LOW);
	if (IS_ERR(priv->ipc_cp_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->ipc_cp_gpio),
				     "failed to get CLIENT WAKE GPIO\n");

	priv->ipc_ap_gpio = devm_gpiod_get_optional(dev, "link-hostwake", GPIOD_IN);
	if (IS_ERR(priv->ipc_ap_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->ipc_ap_gpio),
				     "failed to get HOST WAKE GPIO\n");

	/* USB BUS */
	usb_node = of_parse_phandle(pdev->dev.of_node, "usb-bus", 0);
	if (IS_ERR(usb_node))
		return dev_err_probe(&pdev->dev, PTR_ERR(usb_node),
				     "cannot parse modem USB bus\n");

	priv->usb_dev = of_find_device_by_node(usb_node);
	of_node_put(usb_node);
	if (!priv->usb_dev)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "cannot get modem USB bus\n");

	priv->usb_phy = devm_usb_get_phy_by_phandle(&pdev->dev, "usb-bus", 1);
	if (IS_ERR(priv->usb_phy))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->usb_phy),
				     "failed to get PHY");

	priv->hsic_en_gpio = devm_gpiod_get_optional(dev, "phy-enable", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->hsic_en_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->hsic_en_gpio),
				     "failed to get HSIC EN GPIO\n");

	/* ver 1145 or later starts in READY state */
	/* ap_wake keeps low util CP starts to initiate hsic hw. */
	/* ap_wake goes up during cp hsic init and then */
	/* it goes down when cp hsic ready */
	priv->ap_state = IPC_AP_WAKE_IRQ_READY;

	baseband_xmm_reset(priv);
	gpiod_set_value_cansleep(priv->hsic_en_gpio, 0);

	ret = devm_add_action_or_reset(dev, baseband_xmm_power_off, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add action\n");

	devm_delayed_work_autocancel(dev, &priv->modem_init_work, baseband_xmm_init_work);
	devm_delayed_work_autocancel(dev, &priv->modem_poll_work, baseband_xmm_poll_work);

	priv->irq_hostwake = platform_get_irq(pdev, 0);
	if (priv->irq_hostwake < 0)
		return dev_err_probe(&pdev->dev, priv->irq_hostwake,
				     "failed to get IRQ %d\n", priv->irq_hostwake);

	ret = devm_request_irq(dev, priv->irq_hostwake, baseband_hostwake_interrupt,
			       IRQF_ONESHOT | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       "modem-hostwake", priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IRQ %d\n", priv->irq_hostwake);

//	device_create_file(dev, &dev_attr_ehci_power);
	priv->powered = false;

	return 0;
}

static const struct of_device_id baseband_xmm_power_match[] = {
	{ .compatible = "infineon,xmm6260-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, baseband_xmm_power_match);

static struct platform_driver baseband_xmm_power_driver = {
	.driver = {
		.name = "baseband-xmm-power",
		.of_match_table = baseband_xmm_power_match,
	},
	.probe = baseband_xmm_power_probe,
};
module_platform_driver(baseband_xmm_power_driver);

MODULE_AUTHOR("Svyatolsav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Baseband power supply driver");
MODULE_LICENSE("GPL");
