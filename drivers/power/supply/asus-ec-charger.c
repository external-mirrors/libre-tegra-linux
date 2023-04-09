// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASUS EC driver - charger monitoring
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/mfd/asus-ec.h>
#include <linux/unaligned.h>

struct asusec_charger_data {
	struct notifier_block	 nb;
	const struct asusec_info *ec;
	struct power_supply	*psy;
	struct power_supply_desc psy_desc;
	struct mutex		 ctrl_lock;
	u8			 ctrl_addr;
};

static enum power_supply_property asusec_charger_properties[] = {
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

/* Modded copy of asus_dockram_access_ctl for TF600T and TF701T */
static int asusec_charger_access_ctrl(struct asusec_charger_data *priv,
				      u64 *out, u64 mask, u64 xor)
{
	struct i2c_client *dockram_client = priv->ec->dockram;
	char buf[DOCKRAM_ENTRY_BUFSIZE];
	u64 val;
	int ret;

	mutex_lock(&priv->ctrl_lock);

	ret = asus_dockram_read(dockram_client, priv->ctrl_addr, buf);
	if (ret < 0)
		goto unlock_exit;

	if (buf[0] != ASUSEC_CTL_SIZE) {
		ret = -EPROTO;
		goto unlock_exit;
	}

	val = get_unaligned_le64(buf + 1);

	if (out)
		*out = val;

	if (mask || xor) {
		put_unaligned_le64((val & ~mask) ^ xor, buf + 1);
		ret = asus_dockram_write(dockram_client, priv->ctrl_addr, buf);
	}

unlock_exit:
	mutex_unlock(&priv->ctrl_lock);
	if (ret < 0)
		dev_err(&dockram_client->dev, "Failed to access control flags: %d\n",
			ret);

	return ret;
}

static int asusec_charger_get_ctrl(struct asusec_charger_data *priv, u64 *out)
{
	return asusec_charger_access_ctrl(priv, out, 0, 0);
}

static int asusec_charger_update_ctl(struct asusec_charger_data *priv,
				     u64 mask, u64 xor)
{
	return asusec_charger_access_ctrl(priv, NULL, mask, xor);
}

static int asusec_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct asusec_charger_data *priv = power_supply_get_drvdata(psy);
	enum power_supply_usb_type psu;
	int ret;
	u64 ctl;

	ret = asusec_charger_get_ctrl(priv, &ctl);
	if (ret)
		return ret;

	switch (ctl & (ASUSEC_CTL_FULL_POWER_SOURCE | ASUSEC_CTL_DIRECT_POWER_SOURCE)) {
	case ASUSEC_CTL_FULL_POWER_SOURCE:
		psu = POWER_SUPPLY_USB_TYPE_CDP;	// DOCK
		break;
	case ASUSEC_CTL_DIRECT_POWER_SOURCE:
		psu = POWER_SUPPLY_USB_TYPE_SDP;	// USB
		break;
	case 0:
		psu = POWER_SUPPLY_USB_TYPE_UNKNOWN;	// no power source connected
		break;
	default:
		psu = POWER_SUPPLY_USB_TYPE_ACA;	// power adapter
		break;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = psu != POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return 0;

	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = psu;
		return 0;

	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		if (ctl & ASUSEC_CTL_TEST_DISCHARGE)
			val->intval = POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE;
		else if (ctl & ASUSEC_CTL_USB_CHARGE)
			val->intval = POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
		else
			val->intval = POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE;
		return 0;

	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = priv->ec->model;
		return 0;

	default:
		return -EINVAL;
	}
}

static int asusec_charger_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct asusec_charger_data *priv = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		switch ((enum power_supply_charge_behaviour)val->intval) {
		case POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO:
			return asusec_charger_update_ctl(priv,
				ASUSEC_CTL_TEST_DISCHARGE | ASUSEC_CTL_USB_CHARGE,
				ASUSEC_CTL_USB_CHARGE);

		case POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE:
			return asusec_charger_update_ctl(priv,
				ASUSEC_CTL_TEST_DISCHARGE | ASUSEC_CTL_USB_CHARGE,
				0);

		case POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE:
			return asusec_charger_update_ctl(priv,
				ASUSEC_CTL_TEST_DISCHARGE | ASUSEC_CTL_USB_CHARGE,
				ASUSEC_CTL_TEST_DISCHARGE);
		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static int asusec_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		return true;
	default:
		return false;
	}
}

static const struct power_supply_desc asusec_charger_desc = {
	.name = "asusec-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE),
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA),
	.properties = asusec_charger_properties,
	.num_properties = ARRAY_SIZE(asusec_charger_properties),
	.get_property = asusec_charger_get_property,
	.set_property = asusec_charger_set_property,
	.property_is_writeable = asusec_charger_property_is_writeable,
	.no_thermal = true,
};

static int asusec_charger_notify(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct asusec_charger_data *priv =
		container_of(nb, struct asusec_charger_data, nb);

	switch (action) {
	case ASUSEC_SMI_ACTION(POWER_NOTIFY):
	case ASUSEC_SMI_ACTION(ADAPTER_EVENT):
		power_supply_changed(priv->psy);
		break;
	}

	return NOTIFY_DONE;
}

static int asusec_charger_probe(struct platform_device *pdev)
{
	struct asusec_charger_data *priv;
	struct asusec_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct power_supply_config cfg = {};

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->ec = asusec_cell_to_ec(pdev);

	mutex_init(&priv->ctrl_lock);

	if (pdata)
		priv->ctrl_addr = pdata->ctrl_addr;
	else
		priv->ctrl_addr = ASUSEC_DOCKRAM_CONTROL;

	cfg.of_node = pdev->dev.of_node;
	cfg.drv_data = priv;

	memcpy(&priv->psy_desc, &asusec_charger_desc, sizeof(priv->psy_desc));
	priv->psy_desc.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s-charger",
					     priv->ec->name);

	priv->psy = devm_power_supply_register(&pdev->dev, &priv->psy_desc, &cfg);
	if (IS_ERR(priv->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->psy),
				     "Failed to register power supply\n");

	priv->nb.notifier_call = asusec_charger_notify;
	return devm_asusec_register_notifier(pdev, &priv->nb);
}

static const struct of_device_id asusec_charger_ids[] = {
	{ .compatible = "asus,ec-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, asusec_charger_ids);

static struct platform_driver asusec_charger_driver = {
	.driver.name = "asusec-charger",
	.driver.of_match_table = of_match_ptr(asusec_charger_ids),
	.probe = asusec_charger_probe,
};
module_platform_driver(asusec_charger_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer Pad battery charger driver");
MODULE_LICENSE("GPL");
