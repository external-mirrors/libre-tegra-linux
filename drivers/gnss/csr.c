// SPDX-License-Identifier: GPL-2.0

#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gnss.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/rfkill.h>
#include <linux/serdev.h>

#include "serial.h"

static const struct regulator_bulk_data csr_supplies[] = {
	{ .supply = "vcc" }, { .supply = "vio" },
};

static const char * const csr_clks[] = {
	"tcxo", "lpo", "eclk",
};

struct csr_data {
	struct device *dev;
	struct rfkill *rfkill_dev;

	struct gpio_desc *power_gpio;
	struct gpio_desc *reset_gpio;

	struct regulator_bulk_data *supplies;
	struct clk_bulk_data clks[ARRAY_SIZE(csr_clks)];
};

static int csr_set_active(struct gnss_serial *gserial)
{
	struct csr_data *data = gnss_serial_get_drvdata(gserial);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(csr_supplies), data->supplies);
	if (ret) {
		dev_err(data->dev, "error enabling regulators (%d)\n", ret);
		return ret;
	}

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(csr_clks), data->clks);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(csr_supplies), data->supplies);
		dev_err(data->dev, "failed to enable clocks: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(data->power_gpio, 1);
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(data->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(data->reset_gpio, 0);

	return 0;
}

static int csr_set_standby(struct gnss_serial *gserial)
{
	struct csr_data *data = gnss_serial_get_drvdata(gserial);

	gpiod_set_value_cansleep(data->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(data->power_gpio, 0);

	clk_bulk_disable_unprepare(ARRAY_SIZE(csr_clks), data->clks);

	regulator_bulk_disable(ARRAY_SIZE(csr_supplies), data->supplies);

	return 0;
}

static int csr_set_power(struct gnss_serial *gserial,
			 enum gnss_serial_pm_state state)
{
	switch (state) {
	case GNSS_SERIAL_ACTIVE:
		return csr_set_active(gserial);
	case GNSS_SERIAL_OFF:
	case GNSS_SERIAL_STANDBY:
		return csr_set_standby(gserial);
	}

	return -EINVAL;
}

static int rfkill_csr_set_power(void *data, bool blocked)
{
	struct gnss_serial *gserial = data;

	if (blocked)
		return csr_set_standby(gserial);
	else
		return csr_set_active(gserial);
}

static const struct rfkill_ops csr_gps_ops = {
	.set_block = rfkill_csr_set_power,
};

static const struct gnss_serial_ops csr_gserial_ops = {
	.set_power = csr_set_power,
};

static void csr_free_gserial(void *gserial)
{
	gnss_serial_free(gserial);
}

static void csr_deregister_gserial(void *gserial)
{
	gnss_serial_deregister(gserial);
}

static void csr_free_rfkill(void *gserial)
{
	struct csr_data *data = gnss_serial_get_drvdata(gserial);

	rfkill_unregister(data->rfkill_dev);
	rfkill_destroy(data->rfkill_dev);
	csr_set_standby(gserial);
}

static int csr_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct gnss_serial *gserial;
	struct csr_data *data;
	char *name;
	int ret, i;

	gserial = gnss_serial_allocate(serdev, sizeof(*data));
	if (IS_ERR(gserial))
		return dev_err_probe(dev, PTR_ERR(gserial),
				     "can't allocate GNSS serial\n");

	ret = devm_add_action_or_reset(dev, csr_free_gserial, gserial);
	if (ret)
		return ret;

	gserial->ops = &csr_gserial_ops;
	gserial->gdev->type = GNSS_TYPE_NMEA;

	data = gnss_serial_get_drvdata(gserial);
	data->dev = dev;

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(csr_supplies),
					    csr_supplies, &data->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get supplies\n");

	data->power_gpio = devm_gpiod_get_optional(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(data->power_gpio))
		return dev_err_probe(dev, PTR_ERR(data->power_gpio),
				     "failed to get power GPIO\n");

	data->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(data->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(data->reset_gpio),
				     "failed to get reset GPIO\n");

	for (i = 0; i < ARRAY_SIZE(csr_clks); i++)
		data->clks[i].id = csr_clks[i];

	ret = devm_clk_bulk_get_optional(dev, ARRAY_SIZE(csr_clks), data->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get the clocks\n");

	ret = gnss_serial_register(gserial);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, csr_deregister_gserial, gserial);
	if (ret)
		return ret;

	name = devm_kasprintf(dev, GFP_KERNEL, "gnss%d", serdev->nr);
	if (!name)
		return -ENOMEM;

	data->rfkill_dev = rfkill_alloc(name, dev, RFKILL_TYPE_GPS,
					&csr_gps_ops, gserial);
	if (data->rfkill_dev) {
		ret = rfkill_register(data->rfkill_dev);
		if (ret < 0) {
			rfkill_destroy(data->rfkill_dev);

			return dev_err_probe(dev, ret,
					     "failed to register GNSS rfkill\n");
		}

		ret = devm_add_action_or_reset(dev, csr_free_rfkill, gserial);
		if (ret)
			return ret;
	} else {
		return dev_err_probe(dev, PTR_ERR(data->rfkill_dev),
				     "failed to allocate GNSS rfkill\n");
	}

	dev_info(data->dev, "CSR GSD5T probed\n");

	return 0;
}

static const struct of_device_id csr_of_match[] = {
	{ .compatible = "csr,gsd5t" },
	{ }
};
MODULE_DEVICE_TABLE(of, csr_of_match);

static struct serdev_device_driver csr_driver = {
	.driver	= {
		.name		= "gnss-csr",
		.of_match_table	= csr_of_match,
		.pm		= &gnss_serial_pm_ops,
	},
	.probe	= csr_probe,
};
module_serdev_device_driver(csr_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("CSR GNSS NMEA receiver driver");
MODULE_LICENSE("GPL");
