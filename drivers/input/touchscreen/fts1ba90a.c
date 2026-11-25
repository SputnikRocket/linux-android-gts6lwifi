// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics FTS1BA90A touchscreen controller with Samsung's
 * firmware, as found in the Samsung Galaxy Tab S6 (SM-T860).
 *
 * The firmware speaks the same 8-byte event format as Samsung's own
 * controllers (see s6sy761.c), with 0-based touch IDs and STM's own
 * commands for reset, calibration and scanning. Protocol taken from
 * Samsung's SM-T860 kernel (drivers/input/touchscreen/stm/fts1ba90a).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>

/* Commands */
#define FTS_CMD_FORCE_CALIBRATION	0x13
#define FTS_CMD_READ_DEVICE_ID		0x22
#define FTS_CMD_READ_FW_VERSION		0x24
#define FTS_CMD_SET_TOUCH_TYPE		0x30
#define FTS_CMD_READ_ONE_EVENT		0x60
#define FTS_CMD_READ_ALL_EVENT		0x61
#define FTS_CMD_CLEAR_ALL_EVENT		0x62
#define FTS_CMD_SET_SCAN_MODE		0xa0
#define FTS_CMD_HW_REG			0xfa

#define FTS_SCAN_MODE_OFF		0x00
#define FTS_SCAN_MODE_MS_SS		0x01

/* Touch types to report: finger, palm and wet finger */
#define FTS_TOUCH_TYPES			(BIT(0) | BIT(5) | BIT(6))

#define FTS_DEVICE_ID0			0x39
#define FTS_DEVICE_ID1			0x36

#define FTS_EVENT_SIZE			8
#define FTS_EVENT_FIFO_MAX		32
#define FTS_MAX_FINGERS			10

/* Event byte 0 */
#define FTS_EVENT_EID			GENMASK(1, 0)
#define FTS_EVENT_TID			GENMASK(5, 2)
#define FTS_EVENT_STYPE			GENMASK(5, 2)
#define FTS_EVENT_TCHSTA		GENMASK(7, 6)

#define FTS_EID_COORDINATE		0
#define FTS_EID_STATUS			1

#define FTS_TCHSTA_RELEASE		3

#define FTS_STYPE_ERROR			1
#define FTS_STYPE_INFORMATION		2

#define FTS_INFO_READY			0x00
#define FTS_ERR_QUEUE_FULL		0x01
#define FTS_ERR_ESD			0x02

/* Whole first bytes of the reports that are not coordinate/status events */
#define FTS_EVENT_STATUS_REPORT		0x43	/* byte 1 0x01: command echo */
#define FTS_EVENT_ERROR_REPORT		0xf3

/* Coordinate event */
#define FTS_EVENT_X_LSB			GENMASK(7, 4)	/* byte 3 */
#define FTS_EVENT_Y_LSB			GENMASK(3, 0)	/* byte 3 */
#define FTS_EVENT_Z			GENMASK(5, 0)	/* byte 6 */
#define FTS_EVENT_LEFT			GENMASK(5, 0)	/* byte 7 */

struct fts1ba90a {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator_bulk_data supplies[2];
	struct touchscreen_properties prop;
	u8 events[FTS_EVENT_FIFO_MAX * FTS_EVENT_SIZE];
};

static int fts1ba90a_write(struct fts1ba90a *ts, const u8 *buf, int len)
{
	int ret = i2c_master_send(ts->client, buf, len);

	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int fts1ba90a_read(struct fts1ba90a *ts, const u8 *cmd, int cmd_len,
			  u8 *buf, int len)
{
	struct i2c_msg msgs[] = {
		{
			.addr = ts->client->addr,
			.len = cmd_len,
			.buf = (u8 *)cmd,
		}, {
			.addr = ts->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret = i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));

	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int fts1ba90a_read_one_event(struct fts1ba90a *ts, u8 *event)
{
	static const u8 cmd = FTS_CMD_READ_ONE_EVENT;

	return fts1ba90a_read(ts, &cmd, 1, event, FTS_EVENT_SIZE);
}

/* Wait for the "controller ready" event that follows a reset. */
static int fts1ba90a_wait_ready(struct fts1ba90a *ts)
{
	struct device *dev = &ts->client->dev;
	u8 event[FTS_EVENT_SIZE];
	int i, ret;

	for (i = 0; i < 25; i++) {
		ret = fts1ba90a_read_one_event(ts, event);
		if (ret)
			return ret;

		if (FIELD_GET(FTS_EVENT_EID, event[0]) == FTS_EID_STATUS &&
		    FIELD_GET(FTS_EVENT_STYPE, event[0]) == FTS_STYPE_INFORMATION &&
		    event[1] == FTS_INFO_READY)
			return 0;

		if (event[0] == FTS_EVENT_ERROR_REPORT)
			dev_warn(dev, "error report %8ph\n", event);

		msleep(20);
	}

	return -ETIMEDOUT;
}

/* Send a command and wait for the controller to echo it back. */
static int fts1ba90a_command(struct fts1ba90a *ts, const u8 *cmd, int len)
{
	u8 event[FTS_EVENT_SIZE];
	int i, ret;

	ret = fts1ba90a_write(ts, cmd, len);
	if (ret)
		return ret;

	for (i = 0; i < 100; i++) {
		ret = fts1ba90a_read_one_event(ts, event);
		if (ret)
			return ret;

		if (event[0] == FTS_EVENT_STATUS_REPORT && event[1] == 0x01 &&
		    !memcmp(&event[2], cmd, min(len, FTS_EVENT_SIZE - 2)))
			return 0;

		if (event[0] == FTS_EVENT_ERROR_REPORT)
			return -EIO;

		msleep(20);
	}

	return -ETIMEDOUT;
}

static int fts1ba90a_set_scan_mode(struct fts1ba90a *ts, u8 mode)
{
	const u8 cmd[] = { FTS_CMD_SET_SCAN_MODE, 0x00, mode };

	return fts1ba90a_command(ts, cmd, sizeof(cmd));
}

static int fts1ba90a_system_reset(struct fts1ba90a *ts)
{
	static const u8 cmd[] = { FTS_CMD_HW_REG, 0x20, 0x00, 0x00, 0x24, 0x81 };
	int ret;

	ret = fts1ba90a_write(ts, cmd, sizeof(cmd));
	if (ret)
		return ret;

	usleep_range(10000, 11000);

	return fts1ba90a_wait_ready(ts);
}

static int fts1ba90a_power_on(struct fts1ba90a *ts)
{
	int ret;

	/* AVDD first, then VDD, as the controller wants */
	ret = regulator_enable(ts->supplies[1].consumer);
	if (ret)
		return ret;

	usleep_range(1000, 1500);

	ret = regulator_enable(ts->supplies[0].consumer);
	if (ret) {
		regulator_disable(ts->supplies[1].consumer);
		return ret;
	}

	/* Boot time before the controller reports ready */
	msleep(20);

	return 0;
}

static void fts1ba90a_power_off(struct fts1ba90a *ts)
{
	regulator_disable(ts->supplies[0].consumer);
	regulator_disable(ts->supplies[1].consumer);
}

/* Bring the controller from power-on to scanning-ready. */
static int fts1ba90a_init(struct fts1ba90a *ts, bool calibrate)
{
	static const u8 clear[] = { FTS_CMD_CLEAR_ALL_EVENT };
	static const u8 calib[] = { FTS_CMD_FORCE_CALIBRATION };
	static const u8 types[] = { FTS_CMD_SET_TOUCH_TYPE,
				    FTS_TOUCH_TYPES & 0xff,
				    FTS_TOUCH_TYPES >> 8 };
	struct device *dev = &ts->client->dev;
	int ret;

	ret = fts1ba90a_system_reset(ts);
	if (ret) {
		dev_err(dev, "controller did not come out of reset: %d\n", ret);
		return ret;
	}

	ret = fts1ba90a_write(ts, types, sizeof(types));
	if (ret)
		return ret;

	usleep_range(10000, 11000);

	if (calibrate) {
		ret = fts1ba90a_command(ts, calib, sizeof(calib));
		if (ret)
			dev_warn(dev, "calibration failed: %d\n", ret);
	}

	return fts1ba90a_command(ts, clear, sizeof(clear));
}

static void fts1ba90a_report_coordinate(struct fts1ba90a *ts, const u8 *event)
{
	unsigned int tid = FIELD_GET(FTS_EVENT_TID, event[0]);
	unsigned int x, y;

	if (tid >= FTS_MAX_FINGERS)
		return;

	input_mt_slot(ts->input, tid);

	if (FIELD_GET(FTS_EVENT_TCHSTA, event[0]) == FTS_TCHSTA_RELEASE) {
		input_mt_report_slot_inactive(ts->input);
		return;
	}

	x = (event[1] << 4) | FIELD_GET(FTS_EVENT_X_LSB, event[3]);
	y = (event[2] << 4) | FIELD_GET(FTS_EVENT_Y_LSB, event[3]);

	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
	touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, event[4]);
	input_report_abs(ts->input, ABS_MT_TOUCH_MINOR, event[5]);
	input_report_abs(ts->input, ABS_MT_PRESSURE,
			 max(FIELD_GET(FTS_EVENT_Z, event[6]), 1UL));
}

static void fts1ba90a_handle_status(struct fts1ba90a *ts, const u8 *event)
{
	struct device *dev = &ts->client->dev;

	if (FIELD_GET(FTS_EVENT_STYPE, event[0]) != FTS_STYPE_ERROR)
		return;

	if (event[1] == FTS_ERR_QUEUE_FULL)
		dev_warn_ratelimited(dev, "event queue full\n");
	else if (event[1] == FTS_ERR_ESD)
		dev_warn_ratelimited(dev, "ESD event\n");
}

static irqreturn_t fts1ba90a_irq(int irq, void *data)
{
	static const u8 read_all = FTS_CMD_READ_ALL_EVENT;
	struct fts1ba90a *ts = data;
	unsigned int left, i;
	int ret;

	ret = fts1ba90a_read_one_event(ts, ts->events);
	if (ret) {
		dev_err_ratelimited(&ts->client->dev,
				    "failed to read event: %d\n", ret);
		return IRQ_HANDLED;
	}

	left = FIELD_GET(FTS_EVENT_LEFT, ts->events[7]);
	left = min(left, FTS_EVENT_FIFO_MAX - 1);
	if (left) {
		ret = fts1ba90a_read(ts, &read_all, 1,
				     &ts->events[FTS_EVENT_SIZE],
				     left * FTS_EVENT_SIZE);
		if (ret)
			left = 0;
	}

	for (i = 0; i <= left; i++) {
		const u8 *event = &ts->events[i * FTS_EVENT_SIZE];

		switch (FIELD_GET(FTS_EVENT_EID, event[0])) {
		case FTS_EID_COORDINATE:
			fts1ba90a_report_coordinate(ts, event);
			break;
		case FTS_EID_STATUS:
			fts1ba90a_handle_status(ts, event);
			break;
		}
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);

	return IRQ_HANDLED;
}

static int fts1ba90a_start(struct fts1ba90a *ts)
{
	int ret;

	ret = fts1ba90a_set_scan_mode(ts, FTS_SCAN_MODE_MS_SS);
	if (ret)
		return ret;

	enable_irq(ts->client->irq);

	return 0;
}

static void fts1ba90a_stop(struct fts1ba90a *ts)
{
	disable_irq(ts->client->irq);
	fts1ba90a_set_scan_mode(ts, FTS_SCAN_MODE_OFF);
}

static int fts1ba90a_input_open(struct input_dev *dev)
{
	struct fts1ba90a *ts = input_get_drvdata(dev);

	return fts1ba90a_start(ts);
}

static void fts1ba90a_input_close(struct input_dev *dev)
{
	struct fts1ba90a *ts = input_get_drvdata(dev);

	fts1ba90a_stop(ts);
}

static void fts1ba90a_power_off_action(void *data)
{
	fts1ba90a_power_off(data);
}

static int fts1ba90a_probe(struct i2c_client *client)
{
	static const u8 dev_id_cmd = FTS_CMD_READ_DEVICE_ID;
	static const u8 fw_cmd = FTS_CMD_READ_FW_VERSION;
	struct device *dev = &client->dev;
	struct fts1ba90a *ts;
	u8 id[5], fw[9];
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->supplies[0].supply = "vdd";
	ts->supplies[1].supply = "avdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies),
				      ts->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ret = fts1ba90a_power_on(ts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on\n");

	ret = devm_add_action_or_reset(dev, fts1ba90a_power_off_action, ts);
	if (ret)
		return ret;

	ret = fts1ba90a_init(ts, true);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize\n");

	ret = fts1ba90a_read(ts, &dev_id_cmd, 1, id, sizeof(id));
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");

	if (id[2] != FTS_DEVICE_ID0 && id[3] != FTS_DEVICE_ID1)
		return dev_err_probe(dev, -ENODEV, "unknown device ID %5ph\n", id);

	if (!fts1ba90a_read(ts, &fw_cmd, 1, fw, sizeof(fw)))
		dev_info(dev, "firmware 0x%04x, config 0x%04x\n",
			 (fw[0] << 8) | fw[1], (fw[2] << 8) | fw[3]);

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "STM FTS1BA90A touchscreen";
	ts->input->id.bustype = BUS_I2C;
	ts->input->open = fts1ba90a_input_open;
	ts->input->close = fts1ba90a_input_close;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 4095, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 4095, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 63, 0, 0);

	touchscreen_parse_properties(ts->input, true, &ts->prop);
	if (!ts->prop.max_x || !ts->prop.max_y)
		return dev_err_probe(dev, -EINVAL,
				     "touchscreen-size-x/y are required\n");

	ret = input_mt_init_slots(ts->input, FTS_MAX_FINGERS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	input_set_drvdata(ts->input, ts);

	ret = devm_request_threaded_irq(dev, client->irq, NULL, fts1ba90a_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"fts1ba90a", ts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	return input_register_device(ts->input);
}

static int fts1ba90a_suspend(struct device *dev)
{
	struct fts1ba90a *ts = i2c_get_clientdata(to_i2c_client(dev));

	guard(mutex)(&ts->input->mutex);

	if (input_device_enabled(ts->input))
		fts1ba90a_stop(ts);

	fts1ba90a_power_off(ts);

	return 0;
}

static int fts1ba90a_resume(struct device *dev)
{
	struct fts1ba90a *ts = i2c_get_clientdata(to_i2c_client(dev));
	int ret;

	guard(mutex)(&ts->input->mutex);

	ret = fts1ba90a_power_on(ts);
	if (ret)
		return ret;

	ret = fts1ba90a_init(ts, false);
	if (ret)
		return ret;

	if (input_device_enabled(ts->input))
		return fts1ba90a_start(ts);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fts1ba90a_pm_ops,
				fts1ba90a_suspend, fts1ba90a_resume);

static const struct of_device_id fts1ba90a_of_match[] = {
	{ .compatible = "st,fts1ba90a" },
	{ }
};
MODULE_DEVICE_TABLE(of, fts1ba90a_of_match);

static const struct i2c_device_id fts1ba90a_id[] = {
	{ "fts1ba90a" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fts1ba90a_id);

static struct i2c_driver fts1ba90a_driver = {
	.driver = {
		.name = "fts1ba90a",
		.of_match_table = fts1ba90a_of_match,
		.pm = pm_sleep_ptr(&fts1ba90a_pm_ops),
	},
	.probe = fts1ba90a_probe,
	.id_table = fts1ba90a_id,
};
module_i2c_driver(fts1ba90a_driver);

MODULE_DESCRIPTION("STMicroelectronics FTS1BA90A touchscreen driver");
MODULE_LICENSE("GPL");
