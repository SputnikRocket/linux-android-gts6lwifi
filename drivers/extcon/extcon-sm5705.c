// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5705 MUIC (Micro-USB Interface Controller)
 *
 * The MUIC block of the SM5705 power management IC sits between the D+/D-
 * lines of the USB connector and the SoC, and switches them to the USB PHY,
 * a UART or an audio path. Its automatic switching does not connect the USB
 * path in Type-C designs, so the vendor driver keeps it in manual mode and
 * connects D+/D- to USB itself; at shutdown it opens them again and returns
 * to automatic mode, so the IC can still tell chargers apart while the
 * device is off. Without a driver, USB therefore has no data lines.
 *
 * This driver does only what USB needs: connect D+/D- to the USB PHY while
 * the kernel runs, and hand the switch back to automatic mode at shutdown.
 * It does not detect cables yet. Register layout taken from Samsung's
 * SM-T860 kernel (drivers/muic/universal/muic_regmap_sm5705.c).
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define SM5705_REG_DEVICE_ID		0x01
#define SM5705_REG_CONTROL		0x02
/* Set: automatic switching. Clear: MANUAL_SW1 selects the path. */
#define SM5705_CONTROL_AUTO_SW		BIT(2)
#define SM5705_REG_MANUAL_SW1		0x13
#define SM5705_MANUAL_SW1_DM		GENMASK(7, 5)
#define SM5705_MANUAL_SW1_DP		GENMASK(4, 2)
#define SM5705_REG_MAX			0x37

#define SM5705_SW_OPEN			0x0
#define SM5705_SW_USB			0x1

static const struct regmap_config sm5705_muic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SM5705_REG_MAX,
};

static int sm5705_muic_connect_usb(struct regmap *regmap)
{
	int ret;

	/* Select the path first, so that the switch never sees a stale one */
	ret = regmap_write(regmap, SM5705_REG_MANUAL_SW1,
			   FIELD_PREP(SM5705_MANUAL_SW1_DM, SM5705_SW_USB) |
			   FIELD_PREP(SM5705_MANUAL_SW1_DP, SM5705_SW_USB));
	if (ret)
		return ret;

	return regmap_clear_bits(regmap, SM5705_REG_CONTROL,
				 SM5705_CONTROL_AUTO_SW);
}

static int sm5705_muic_release(struct regmap *regmap)
{
	int ret;

	ret = regmap_write(regmap, SM5705_REG_MANUAL_SW1,
			   FIELD_PREP(SM5705_MANUAL_SW1_DM, SM5705_SW_OPEN) |
			   FIELD_PREP(SM5705_MANUAL_SW1_DP, SM5705_SW_OPEN));
	if (ret)
		return ret;

	return regmap_set_bits(regmap, SM5705_REG_CONTROL,
			       SM5705_CONTROL_AUTO_SW);
}

static void sm5705_muic_release_action(void *data)
{
	sm5705_muic_release(data);
}

static int sm5705_muic_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	unsigned int id;
	int ret;

	regmap = devm_regmap_init_i2c(client, &sm5705_muic_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to initialise regmap\n");

	ret = regmap_read(regmap, SM5705_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read the device ID\n");

	ret = sm5705_muic_connect_usb(regmap);
	if (ret)
		return dev_err_probe(dev, ret, "failed to connect D+/D- to USB\n");

	ret = devm_add_action_or_reset(dev, sm5705_muic_release_action, regmap);
	if (ret)
		return ret;

	i2c_set_clientdata(client, regmap);
	dev_info(dev, "device ID %#04x, D+/D- connected to USB\n", id);

	return 0;
}

static void sm5705_muic_shutdown(struct i2c_client *client)
{
	struct regmap *regmap = i2c_get_clientdata(client);

	if (regmap)
		sm5705_muic_release(regmap);
}

static const struct of_device_id sm5705_muic_of_match[] = {
	{ .compatible = "siliconmitus,sm5705-muic" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5705_muic_of_match);

static const struct i2c_device_id sm5705_muic_id[] = {
	{ "sm5705-muic" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm5705_muic_id);

static struct i2c_driver sm5705_muic_driver = {
	.driver = {
		.name = "sm5705-muic",
		.of_match_table = sm5705_muic_of_match,
	},
	.probe = sm5705_muic_probe,
	.shutdown = sm5705_muic_shutdown,
	.id_table = sm5705_muic_id,
};
module_i2c_driver(sm5705_muic_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5705 MUIC USB path driver");
MODULE_LICENSE("GPL");
