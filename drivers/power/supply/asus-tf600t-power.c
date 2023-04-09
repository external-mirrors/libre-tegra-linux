// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/gpio/consumer.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/asus-transformer-ec.h> // remove if conflicts
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define ASUSEC_RSP_BUFFER_SIZE			8

#define ASUSEC_DOCKRAM_CHARGER_CTL		0x23
#define ASUSEC_DOCKRAM_BATTERY_CTL		0x24

#define ASUSEC_BATTERY_DATA_FRESH_MSEC		5000

#define ASUSEC_BATTERY_DISCHARGING		0x40
#define ASUSEC_BATTERY_FULL_CHARGED		0x20
#define ASUSEC_BATTERY_NOT_CHARGING		0x10

#define TEMP_CELSIUS_OFFSET			2731

struct asus_ec_power_data {
	struct i2c_client *client;
	struct i2c_client *dockram;
	struct mutex ctl_lock; /* prevent simultaneous access */
	const char *model;

	/* battery */
	struct power_supply *battery;
	struct delayed_work poll_work;
	char ec_data[DOCKRAM_ENTRY_BUFSIZE];
	unsigned long batt_data_ts;
	int last_state;

	/* charger */
	struct blocking_notifier_head notify_list;
	struct notifier_block nb;
	struct power_supply *charger;
	char ctl_data[DOCKRAM_ENTRY_BUFSIZE];
};

static enum power_supply_property asus_ec_power_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_PRESENT,
};

static enum power_supply_property asus_ec_power_charger_properties[] = {
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const unsigned int asus_ec_power_battery_prop_offs[] = {
	[POWER_SUPPLY_PROP_STATUS] = 1,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = 3,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = 5,
	[POWER_SUPPLY_PROP_TEMP] = 7,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = 9,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = 11,
	[POWER_SUPPLY_PROP_CAPACITY] = 13,
	[POWER_SUPPLY_PROP_CHARGE_NOW] = 15,
	[POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW] = 17,
	[POWER_SUPPLY_PROP_TIME_TO_FULL_NOW] = 19,
};

static int asus_ec_power_dockram_read(struct asus_ec_power_data *priv, int reg,
				      char *buf)
{
	struct device *dev = &priv->client->dev;

	int ret;

	memset(buf, 0, DOCKRAM_ENTRY_BUFSIZE);
	ret = i2c_smbus_read_i2c_block_data(priv->dockram, reg,
					    DOCKRAM_ENTRY_BUFSIZE, buf);
	if (ret < 0)
		return ret;

	if (buf[0] > DOCKRAM_ENTRY_SIZE) {
		dev_err(dev, "bad data len; buffer: %*ph; ret: %d\n",
			DOCKRAM_ENTRY_BUFSIZE, buf, ret);
		return -EPROTO;
	}

	dev_dbg(dev, "got data; buffer: %*ph; ret: %d\n",
		DOCKRAM_ENTRY_BUFSIZE, buf, ret);

	return 0;
}

static int asus_ec_power_dockram_write(struct asus_ec_power_data *priv, int reg,
				       const char *buf)
{
	if (buf[0] > DOCKRAM_ENTRY_SIZE)
		return -EINVAL;

	dev_dbg(&priv->client->dev, "sending data; buffer: %*ph\n", buf[0] + 1, buf);

	return i2c_smbus_write_i2c_block_data(priv->dockram, reg, buf[0] + 1, buf);
}

static int asus_ec_power_dockram_access_ctl(struct asus_ec_power_data *priv, u64 *out,
					    u64 mask, u64 xor)
{
	char *buf = priv->ctl_data;
	u64 val;
	int ret = 0;

	guard(mutex)(&priv->ctl_lock);

	ret = asus_ec_power_dockram_read(priv, ASUSEC_DOCKRAM_CHARGER_CTL, buf);
	if (ret < 0)
		goto exit;

	if (buf[0] != ASUSEC_CTL_SIZE) {
		ret = -EPROTO;
		goto exit;
	}

	val = get_unaligned_le64(buf + 1);

	if (out)
		*out = val;

	if (mask || xor) {
		put_unaligned_le64((val & ~mask) ^ xor, buf + 1);
		ret = asus_ec_power_dockram_write(priv, ASUSEC_DOCKRAM_CHARGER_CTL, buf);
	}

exit:
	if (ret < 0)
		dev_err(&priv->client->dev, "Failed to access control flags: %d\n",
			ret);

	return ret;
}

static int asus_ec_power_info_get(struct asus_ec_power_data *priv, int reg, char *buf)
{
	int ret, i;
	u8 command[] = { 0x05, 0x0b, 0x00, 0x36, (u8)reg, 0x18 };

	ret = asus_ec_power_dockram_write(priv, 0x11, command);
	if (ret < 0)
		return ret;

	msleep(20);

	ret = asus_ec_power_dockram_read(priv, 0x11, buf);
	if (ret < 0)
	        return ret;

	/* shift data left by 9 */
	for (i = 9; i < 32; i++)
		buf[i-9] = buf[i];

	return 0;
}

static int asus_ec_power_charger_get_ctrl(struct asus_ec_power_data *priv, u64 *out)
{
	return asus_ec_power_dockram_access_ctl(priv, out, 0, 0);
}

static int asus_ec_power_charger_update_ctl(struct asus_ec_power_data *priv,
					    u64 mask, u64 xor)
{
	return asus_ec_power_dockram_access_ctl(priv, NULL, mask, xor);
}

static int asus_ec_power_battery_refresh(struct asus_ec_power_data *priv)
{
	int ret = 0;

	guard(mutex)(&priv->ctl_lock);

	if (time_before(jiffies, priv->batt_data_ts))
		return ret;

	ret = asus_ec_power_dockram_read(priv, ASUSEC_DOCKRAM_BATTERY_CTL,
					 priv->ec_data);
	if (ret < 0)
		return ret;

	priv->batt_data_ts = jiffies +
		msecs_to_jiffies(ASUSEC_BATTERY_DATA_FRESH_MSEC);

	return ret;
}

static int asus_ec_power_battery_get_value(struct asus_ec_power_data *priv,
					   enum power_supply_property psp)
{
	int ret, offs;

	if (psp >= ARRAY_SIZE(asus_ec_power_battery_prop_offs))
		return -EINVAL;

	offs = asus_ec_power_battery_prop_offs[psp];
	if (!offs)
		return -EINVAL;

	ret = asus_ec_power_battery_refresh(priv);
	if (ret < 0)
		return ret;

	if (offs >= priv->ec_data[0])
		return -ENODATA;

	return get_unaligned_le16(priv->ec_data + offs);
}

static int asus_ec_power_battery_get_property(struct power_supply *psy,
					      enum power_supply_property psp,
					      union power_supply_propval *val)
{
	struct asus_ec_power_data *priv = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;

	default:
		ret = asus_ec_power_battery_get_value(priv, psp);
		if (ret < 0)
			return ret;

		val->intval = (s16)ret;

		switch (psp) {
		case POWER_SUPPLY_PROP_STATUS:
			if (ret & ASUSEC_BATTERY_FULL_CHARGED)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else if (ret & ASUSEC_BATTERY_NOT_CHARGING)
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			else if (ret & ASUSEC_BATTERY_DISCHARGING)
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			break;

		case POWER_SUPPLY_PROP_TEMP:
			val->intval -= TEMP_CELSIUS_OFFSET;
			break;

		case POWER_SUPPLY_PROP_CHARGE_NOW:
		case POWER_SUPPLY_PROP_CURRENT_NOW:
		case POWER_SUPPLY_PROP_CURRENT_MAX:
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		case POWER_SUPPLY_PROP_VOLTAGE_MAX:
			val->intval *= 1000;
			break;

		case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
			val->intval *= 60;
			break;

		default:
			break;
		}

		break;
	}

	return 0;
}

static void asus_ec_power_battery_poll_work(struct work_struct *work)
{
	struct asus_ec_power_data *priv =
		container_of(work, struct asus_ec_power_data, poll_work.work);
	int state;

	state = asus_ec_power_battery_get_value(priv, POWER_SUPPLY_PROP_STATUS);
	if (state < 0)
		return;

	if (state & ASUSEC_BATTERY_FULL_CHARGED)
		state = POWER_SUPPLY_STATUS_FULL;
	else if (state & ASUSEC_BATTERY_DISCHARGING)
		state = POWER_SUPPLY_STATUS_DISCHARGING;
	else
		state = POWER_SUPPLY_STATUS_CHARGING;

	if (priv->last_state != state) {
		priv->last_state = state;
		power_supply_changed(priv->battery);
	}

	/* continuously send uevent notification */
	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_BATTERY_DATA_FRESH_MSEC));
}

static const struct power_supply_desc asus_ec_power_battery_desc = {
	.name = "dock-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = asus_ec_power_battery_properties,
	.num_properties = ARRAY_SIZE(asus_ec_power_battery_properties),
	.get_property = asus_ec_power_battery_get_property,
	.external_power_changed = power_supply_changed,
};

static int asus_ec_power_charger_get_property(struct power_supply *psy,
					      enum power_supply_property psp,
					      union power_supply_propval *val)
{
	struct asus_ec_power_data *priv = power_supply_get_drvdata(psy);
	enum power_supply_usb_type psu;
	int ret;
	u64 ctl;

	ret = asus_ec_power_charger_get_ctrl(priv, &ctl);
	if (ret)
		return ret;

	switch (ctl & (ASUSEC_CTL_FULL_POWER_SOURCE | ASUSEC_CTL_DIRECT_POWER_SOURCE)) {
	case ASUSEC_CTL_FULL_POWER_SOURCE:
		psu = POWER_SUPPLY_USB_TYPE_CDP;	/* DOCK */
		break;
	case ASUSEC_CTL_DIRECT_POWER_SOURCE:
		psu = POWER_SUPPLY_USB_TYPE_SDP;	/* USB */
		break;
	case 0:
		psu = POWER_SUPPLY_USB_TYPE_UNKNOWN;	/* no power source connected */
		break;
	default:
		psu = POWER_SUPPLY_USB_TYPE_ACA;	/* power adapter */
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
		val->strval = priv->model;
		return 0;

	default:
		return -EINVAL;
	}
}

static int asus_ec_power_charger_set_property(struct power_supply *psy,
					      enum power_supply_property psp,
					      const union power_supply_propval *val)
{
	struct asus_ec_power_data *priv = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		switch ((enum power_supply_charge_behaviour)val->intval) {
		case POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO:
			return asus_ec_power_charger_update_ctl(priv,
				ASUSEC_CTL_TEST_DISCHARGE | ASUSEC_CTL_USB_CHARGE,
				ASUSEC_CTL_USB_CHARGE);

		case POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE:
			return asus_ec_power_charger_update_ctl(priv,
				ASUSEC_CTL_TEST_DISCHARGE | ASUSEC_CTL_USB_CHARGE,
				0);

		case POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE:
			return asus_ec_power_charger_update_ctl(priv,
				ASUSEC_CTL_TEST_DISCHARGE | ASUSEC_CTL_USB_CHARGE,
				ASUSEC_CTL_TEST_DISCHARGE);
		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static int asus_ec_power_charger_property_is_writeable(struct power_supply *psy,
						       enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		return true;
	default:
		return false;
	}
}

static const struct power_supply_desc asus_ec_power_charger_desc = {
	.name = "dock-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE),
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA),
	.properties = asus_ec_power_charger_properties,
	.num_properties = ARRAY_SIZE(asus_ec_power_charger_properties),
	.get_property = asus_ec_power_charger_get_property,
	.set_property = asus_ec_power_charger_set_property,
	.property_is_writeable = asus_ec_power_charger_property_is_writeable,
	.no_thermal = true,
};

static int asus_ec_power_charger_notify(struct notifier_block *nb,
					unsigned long action, void *data)
{
	struct asus_ec_power_data *priv =
		container_of(nb, struct asus_ec_power_data, nb);

	switch (action) {
	case ASUSEC_SMI_ACTION(POWER_NOTIFY):
	case ASUSEC_SMI_ACTION(ADAPTER_EVENT):
		power_supply_changed(priv->charger);
		break;
	}

	return NOTIFY_DONE;
}

static int asus_ec_power_log_info(struct asus_ec_power_data *priv, unsigned int reg,
				  const char *name, char **out)
{
	char *buf = priv->ec_data;
	int i, ret;

	/*
	 * When reading EC data often occures corruption and buffer
	 * is filled with 0xff, reason of this is unknown. After
	 * reading few times (no more then 6) buffer does not
	 * corrupt anymore.
	 */
	for (i = 0; i < DOCKRAM_ENTRY_BUFSIZE; i++) {
		ret = asus_ec_power_info_get(priv, reg, buf);
		if (ret < 0)
			return ret;

		if (buf[0] != 0xFF)
			break;
	}

	if (buf[0] == 0xFF)
		return -EINVAL;

	dev_info(&priv->client->dev, "%-14s: %.*s\n", name, buf[0], buf);

	if (out)
		*out = kstrndup(buf, buf[0], GFP_KERNEL);

	return 0;
}

static int asus_ec_power_detect(struct asus_ec_power_data *priv)
{
	char *model = NULL;
	int ret;

	ret = asus_ec_power_log_info(priv, ASUSEC_DOCKRAM_INFO_MODEL,
				     "model", &model);
	if (ret)
		goto err_exit;

	ret = asus_ec_power_log_info(priv, ASUSEC_DOCKRAM_INFO_FW,
				     "FW version", NULL);
	if (ret)
		goto err_exit;

	ret = asus_ec_power_log_info(priv, ASUSEC_DOCKRAM_INFO_CFGFMT,
				     "Config format", NULL);
	if (ret)
		goto err_exit;

	ret = asus_ec_power_log_info(priv, ASUSEC_DOCKRAM_INFO_HW,
				     "HW version", NULL);
	if (ret)
		goto err_exit;

	priv->model = model;

err_exit:
	if (ret)
		dev_err(&priv->client->dev, "failed to access EC: %d\n", ret);

	kfree(model);

	return ret;
}

static void asus_ec_power_remove_notifier(struct device *dev, void *res)
{
	struct asus_ec_power_data *priv = dev_get_drvdata(dev);
	struct notifier_block **nb = res;

	blocking_notifier_chain_unregister(&priv->notify_list, *nb);
}

static void asus_ec_power_release_dockram(void *client)
{
	i2c_unregister_device(client);
}

static int asus_ec_power_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct asus_ec_power_data *priv;
	struct power_supply_config cfg = { };
	struct notifier_block **res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	i2c_set_clientdata(client, priv);
	priv->client = client;

	priv->dockram = i2c_new_ancillary_device(client, "dockram",
						 client->addr - 2);
	if (IS_ERR(priv->dockram))
		return PTR_ERR(priv->dockram);

	ret = devm_add_action_or_reset(dev, asus_ec_power_release_dockram,
				       priv->dockram);
	if (ret)
		return ret;

	BLOCKING_INIT_NOTIFIER_HEAD(&priv->notify_list);
	mutex_init(&priv->ctl_lock);

	ret = asus_ec_power_detect(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to detect EC\n");

	priv->batt_data_ts = jiffies - 1;
	priv->last_state = POWER_SUPPLY_STATUS_UNKNOWN;

	cfg.fwnode = dev_fwnode(dev->parent);
	cfg.drv_data = priv;

	priv->battery = devm_power_supply_register(dev, &asus_ec_power_battery_desc, &cfg);
	if (IS_ERR(priv->battery))
		return dev_err_probe(dev, PTR_ERR(priv->battery),
				     "Failed to register battery\n");

	ret = devm_delayed_work_autocancel(dev, &priv->poll_work,
					   asus_ec_power_battery_poll_work);
	if (ret)
		return ret;

	priv->charger = devm_power_supply_register(dev, &asus_ec_power_charger_desc, &cfg);
	if (IS_ERR(priv->charger))
		return dev_err_probe(dev, PTR_ERR(priv->charger),
				     "Failed to register charger\n");

	priv->nb.notifier_call = asus_ec_power_charger_notify;

	res = devres_alloc(asus_ec_power_remove_notifier, sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	*res = &priv->nb;
	ret = blocking_notifier_chain_register(&priv->notify_list, &priv->nb);
	if (ret) {
		devres_free(res);
		return ret;
	}

	devres_add(dev, res);

	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_BATTERY_DATA_FRESH_MSEC));

	return 0;
}

static const struct of_device_id asus_ec_power_match[] = {
	{ .compatible = "asus,tf600t-ec-dock" },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_ec_power_match);

static struct i2c_driver asus_ec_power_driver = {
	.driver = {
		.name = "asus-tf600t-power",
		.of_match_table	= asus_ec_power_match,
	},
	.probe = asus_ec_power_probe,
};
module_i2c_driver(asus_ec_power_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("ASUS Transformer TF600T power supply EC driver");
MODULE_LICENSE("GPL");
