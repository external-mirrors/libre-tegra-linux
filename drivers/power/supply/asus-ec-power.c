// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS EC power driver
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mfd/asus-ec.h>

struct asus_ec_power_data {
	struct asusec_info info;
	struct i2c_client *client;
	u8 ec_data[DOCKRAM_ENTRY_BUFSIZE];
};

struct asus_ec_power_initdata {
	const char *model;
};

static struct asus_ec_platform_data asus_ec_pdata = {
	.batt_addr = 0x24,
	.ctrl_addr = 0x23,
};

static const struct asus_ec_power_initdata asus_ec_model_info[] = {
	{	/* Asus T30 Windows Mobile Dock  */
		.model		= "ASUS-TF600T-DOCK",
	},
	{	/* Asus T114 Mobile Dock */
		.model		= "ASUS-TF701T-DOCK",
	},
};

static int asus_ec_power_read(struct i2c_client *client, int reg, char *buf)
{
	int ret, i;
	u8 command[] = { 0x05, 0x0b, 0x00, 0x36, (u8)reg, 0x18 };

	ret = asus_dockram_write(client, 0x11, command);
	if (ret < 0)
		return ret;

	msleep(20);

	ret = asus_dockram_read(client, 0x11, buf);
	if (ret < 0)
	        return ret;

	/* shift data left by 9 */
	for (i = 9; i < 32; i++)
		buf[i-9] = buf[i];

	return 0;
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
		ret = asus_ec_power_read(priv->info.dockram, reg, buf);
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

static void asus_ec_remove_subdev(void *subdev)
{
	platform_device_unregister(subdev);
}

static int asus_ec_power_init_components(struct asus_ec_power_data *priv)
{
	struct device *dev = &priv->client->dev;
	struct platform_device_info subdev_info;
	struct platform_device *subdev;
	struct fwnode_handle *child;
	int ret;

	memset(&subdev_info, 0, sizeof(subdev_info));
	subdev_info.parent = dev;
	subdev_info.id = PLATFORM_DEVID_AUTO;
	subdev_info.data = &asus_ec_pdata;
	subdev_info.size_data = sizeof(asus_ec_pdata);

	device_for_each_child_node(dev, child) {
		if (!fwnode_device_is_available(child))
			continue;

		subdev_info.fwnode = child;
		subdev_info.name = fwnode_get_name(child);
		subdev = platform_device_register_full(&subdev_info);
		if (IS_ERR(subdev))
			return dev_err_probe(dev, PTR_ERR(subdev),
					     "register subdev for %s",
					     subdev_info.name);

		ret = devm_add_action_or_reset(dev, asus_ec_remove_subdev, subdev);
		if (ret)
			return dev_err_probe(dev, ret,
					     "register subdev cleanup action for %s",
					     subdev_info.name);
	}

	return 0;
}

static int asus_ec_power_detect(struct asus_ec_power_data *priv)
{
	char *model = NULL;
	int ret, i;

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

	for (i = 0; i < ARRAY_SIZE(asus_ec_model_info); i++) {
		ret = strcmp(model, asus_ec_model_info[i].model);
		if (!ret) {
			priv->info.model = model;
			priv->info.name = "dock";

			break;
		}
	}

err_exit:
	if (ret)
		dev_err(&priv->client->dev, "failed to access EC: %d\n", ret);

	kfree(model);
	return ret;
}

static int asus_ec_power_probe(struct i2c_client *client)
{
	struct asus_ec_power_data *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	i2c_set_clientdata(client, priv);
	priv->client = client;

	priv->info.dockram = devm_asus_dockram_get(&client->dev);
	if (IS_ERR(priv->info.dockram))
		return dev_err_probe(&client->dev, PTR_ERR(priv->info.dockram),
				     "failed to get dockram\n");

	ret = asus_ec_power_detect(priv);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "EC model not recognized\n");

	ret = asus_ec_power_init_components(priv);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to init components\n");

	return 0;
}

static const struct of_device_id asus_ec_power_match[] = {
	{ .compatible = "asus,ec-power" },
	{ },
};
MODULE_DEVICE_TABLE(of, asus_ec_power_match);

static struct i2c_driver asus_ec_power_driver = {
	.driver = {
		.name = "asus-ec-power",
		.of_match_table	= asus_ec_power_match,
	},
	.probe = asus_ec_power_probe,
};
module_i2c_driver(asus_ec_power_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("ASUS Transformer fuel gauge EC driver");
MODULE_LICENSE("GPL");
