// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include "fm34ne-dsp.h"

struct fm34ne_dsp_data {
	struct i2c_client *client;

	struct gpio_desc *bypass_gpio;
	struct gpio_desc *reset_gpio;

	struct clk *dap_mclk;
	struct regulator *vdd_supply;

	const struct fm34ne_dsp_devdata *data;
};

static int fm34ne_dsp_set_config(struct fm34ne_dsp_data *fm34, int state)
{
	struct device *dev = &fm34->client->dev;
	int ret;

	gpiod_set_value_cansleep(fm34->bypass_gpio, 1);
	msleep(20);

	switch (state) {
	case FM34NE_NS_ENABLE:
		ret = i2c_master_send(fm34->client, fm34->data->enable_parameter,
				      fm34->data->enable_parameter_length);
		if (ret < 0) {
			dev_err(dev, "failed to set DSP enable %d\n", ret);
			goto exit;
		}

		ret = i2c_master_send(fm34->client, fm34->data->enable_noise_suppression,
				      fm34->data->enable_ns_length);
		if (ret < 0) {
			dev_err(dev, "failed to enable DSP noise suppression %d\n", ret);
			goto exit;
		}

		dev_info(dev, "noise suppression enable DSP parameter written\n");
		break;

	case FM34NE_NS_DISABLE:
		ret = i2c_master_send(fm34->client, fm34->data->enable_parameter,
				      fm34->data->enable_parameter_length);
		if (ret < 0) {
			dev_err(dev, "failed to set DSP enable with %d\n", ret);
			goto exit;
		}

		ret = i2c_master_send(fm34->client, fm34->data->disable_noise_suppression,
				      fm34->data->disable_ns_length);
		if (ret < 0) {
			dev_err(dev, "failed to disable DSP noise suppression with %d\n", ret);
			goto exit;
		}

		dev_info(dev, "noise suppression disable DSP parameter written\n");
		break;

	case FM34NE_BYPASS:
	default:
		ret = i2c_master_send(fm34->client, bypass_parameter, sizeof(bypass_parameter));
		if (ret < 0) {
			dev_err(dev, "failed to set DSP bypass with %d\n", ret);
			goto exit;
		}

		dev_info(dev, "bypass DSP parameter written\n");
		break;
	}

exit:
	gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

	return ret;
}

static int fm34ne_dsp_set_hw(struct fm34ne_dsp_data *fm34)
{
	struct device *dev = &fm34->client->dev;
	int ret;

	ret = clk_prepare_enable(fm34->dap_mclk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable the DSP MCLK\n");

	ret = regulator_enable(fm34->vdd_supply);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable power supply\n");

	return 0;
}

static void fm34ne_dsp_reset(struct fm34ne_dsp_data *fm34)
{
	gpiod_set_value_cansleep(fm34->reset_gpio, 1);
	msleep(20);

	gpiod_set_value_cansleep(fm34->reset_gpio, 0);
	msleep(100);
}

static int fm34ne_dsp_init_chip(struct fm34ne_dsp_data *fm34)
{
	int ret;

	ret = fm34ne_dsp_set_hw(fm34);
	if (ret)
		return ret;

	fm34ne_dsp_reset(fm34);

	gpiod_set_value_cansleep(fm34->bypass_gpio, 1);
	msleep(20);

	ret = i2c_smbus_write_byte(fm34->client, FM34NE_BUF_ADDR);
	if (ret < 0) {
		dev_err(&fm34->client->dev, "initial write failed, aborting\n");
		msleep(50);

		fm34ne_dsp_reset(fm34);
		gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

		return ret;
	}

	ret = i2c_master_send(fm34->client, fm34->data->input_parameter,
			      fm34->data->input_parameter_length);
	if (ret < 0)
		return -EINVAL;

	msleep(100);
	gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

	/* Constantly set DSP to bypass mode for now */
	ret = fm34ne_dsp_set_config(fm34, FM34NE_BYPASS);
	if (ret < 0)
		return ret;

	return 0;
}

static int fm34ne_dsp_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fm34ne_dsp_data *fm34;
	int ret;

	fm34 = devm_kzalloc(dev, sizeof(*fm34), GFP_KERNEL);
	if (!fm34)
		return -ENOMEM;

	i2c_set_clientdata(client, fm34);
	fm34->client = client;

	fm34->dap_mclk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(fm34->dap_mclk))
		return dev_err_probe(dev, PTR_ERR(fm34->dap_mclk),
				     "can't retrieve DSP mclk\n");

	fm34->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(fm34->vdd_supply))
		return dev_err_probe(dev, PTR_ERR(fm34->vdd_supply),
				     "failed to get vdd regulator\n");

	fm34->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(fm34->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(fm34->reset_gpio),
				     "failed to get reset GPIO\n");

	/*
	 * Bypass gpio is used to set audio into bypass mode in relation to dsp
	 * to be able to program it. Once programming is done, bypass gpio has
	 * to be set to low to return dsp into audio processing.
	 */
	fm34->bypass_gpio = devm_gpiod_get_optional(dev, "bypass", GPIOD_OUT_LOW);
	if (IS_ERR(fm34->bypass_gpio))
		return dev_err_probe(dev, PTR_ERR(fm34->bypass_gpio),
				     "failed to get bypass GPIO\n");

	fm34->data = of_device_get_match_data(dev);
	if (!fm34->data)
		return -ENODEV;

	dev_info(&fm34->client->dev, "%s detected\n", fm34->data->model);

	ret = fm34ne_dsp_init_chip(fm34);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init DSP chip\n");

	return 0;
}

static int fm34ne_dsp_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fm34ne_dsp_data *fm34 = i2c_get_clientdata(client);

	gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

	regulator_disable(fm34->vdd_supply);

	clk_disable_unprepare(fm34->dap_mclk);

	return 0;
}

static int fm34ne_dsp_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fm34ne_dsp_data *fm34 = i2c_get_clientdata(client);
	int ret;

	ret = fm34ne_dsp_init_chip(fm34);
	if (ret)
		dev_err(&client->dev, "failed to re-init DSP chip with %d\n", ret);

	return 0;
}

static SIMPLE_DEV_PM_OPS(fm34ne_dsp_pm_ops, fm34ne_dsp_suspend, fm34ne_dsp_resume);

static const struct fm34ne_dsp_devdata tf101_dsp_data = {
	.model = "ASUS Eee Pad Transformer TF101",
	.enable_parameter = enable_parameter,
	.enable_parameter_length = sizeof(enable_parameter),
	.input_parameter = tf101_input_parameter,
	.input_parameter_length = sizeof(tf101_input_parameter),
	.enable_noise_suppression = tf101_enable_ns,
	.enable_ns_length = sizeof(tf101_enable_ns),
	.disable_noise_suppression = tf101_disable_ns,
	.disable_ns_length = sizeof(tf101_disable_ns),
};

static const struct fm34ne_dsp_devdata sl101_dsp_data = {
	.model = "ASUS Eee Pad Slider SL101",
	.enable_parameter = sl101_enable_parameter,
	.enable_parameter_length = sizeof(sl101_enable_parameter),
	.input_parameter = sl101_input_parameter,
	.input_parameter_length = sizeof(sl101_input_parameter),
	.enable_noise_suppression = tf101_enable_ns,
	.enable_ns_length = sizeof(tf101_enable_ns),
	.disable_noise_suppression = tf101_disable_ns,
	.disable_ns_length = sizeof(tf101_disable_ns),
};

static const struct fm34ne_dsp_devdata tf201_dsp_data = {
	.model = "ASUS Transformer Prime TF201",
	.enable_parameter = enable_parameter,
	.enable_parameter_length = sizeof(enable_parameter),
	.input_parameter = tf201_input_parameter,
	.input_parameter_length = sizeof(tf201_input_parameter),
	.enable_noise_suppression = tf201_enable_ns,
	.enable_ns_length = sizeof(tf201_enable_ns),
	.disable_noise_suppression = tf101_disable_ns,
	.disable_ns_length = sizeof(tf101_disable_ns),
};

static const struct fm34ne_dsp_devdata tf300t_dsp_data = {
	.model = "ASUS Transformer PAD TF300T",
	.enable_parameter = enable_parameter,
	.enable_parameter_length = sizeof(enable_parameter),
	.input_parameter = tf300t_input_parameter,
	.input_parameter_length = sizeof(tf300t_input_parameter),
	.enable_noise_suppression = tf201_enable_ns,
	.enable_ns_length = sizeof(tf201_enable_ns),
	.disable_noise_suppression = tf101_disable_ns,
	.disable_ns_length = sizeof(tf101_disable_ns),
};

static const struct fm34ne_dsp_devdata tf700t_dsp_data = {
	.model = "ASUS Transformer Infinity TF700T",
	.enable_parameter = enable_parameter,
	.enable_parameter_length = sizeof(enable_parameter),
	.input_parameter = tf700t_input_parameter,
	.input_parameter_length = sizeof(tf700t_input_parameter),
	.enable_noise_suppression = tf700t_enable_ns,
	.enable_ns_length = sizeof(tf700t_enable_ns),
	.disable_noise_suppression = tf700t_disable_ns,
	.disable_ns_length = sizeof(tf700t_disable_ns),
};

static const struct fm34ne_dsp_devdata chagall_dsp_data = {
	.model = "Pegatron Chagall",
	.enable_parameter = enable_parameter,
	.enable_parameter_length = sizeof(enable_parameter),
	.input_parameter = tf300t_input_parameter,
	.input_parameter_length = sizeof(tf300t_input_parameter),
	.enable_noise_suppression = tf201_enable_ns,
	.enable_ns_length = sizeof(tf201_enable_ns),
	.disable_noise_suppression = tf101_disable_ns,
	.disable_ns_length = sizeof(tf101_disable_ns),
};

static const struct of_device_id fm34ne_dsp_match_ids[] = {
	{ .compatible = "asus,tf101-dsp", .data = &tf101_dsp_data },
	{ .compatible = "asus,sl101-dsp", .data = &sl101_dsp_data },
	{ .compatible = "asus,tf201-dsp", .data = &tf201_dsp_data },
	{ .compatible = "asus,tf300t-dsp", .data = &tf300t_dsp_data },
	{ .compatible = "asus,tf700t-dsp", .data = &tf700t_dsp_data },
	{ .compatible = "pegatron,chagall-dsp", .data = &chagall_dsp_data },
	{ }
};
MODULE_DEVICE_TABLE(of, fm34ne_dsp_match_ids);

static const struct i2c_device_id fm34ne_dsp_id[] = {
	{ "dsp_fm34ne" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fm34ne_dsp_id);

static struct i2c_driver fm34ne_dsp_driver = {
	.driver = {
		.name = "fm34ne-dsp",
		.of_match_table = fm34ne_dsp_match_ids,
		.pm = &fm34ne_dsp_pm_ops,
	},
	.id_table = fm34ne_dsp_id,
	.probe = fm34ne_dsp_probe,
};
module_i2c_driver(fm34ne_dsp_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Fortemedia FM34NE DSP driver");
MODULE_LICENSE("GPL");
