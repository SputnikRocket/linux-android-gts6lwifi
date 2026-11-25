// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung AMSA05RB06 10.5" 2560x1600 AMOLED panel with an Anapass ANA38401
 * display driver IC, as found in the Samsung Galaxy Tab S6 (SM-T860).
 *
 * The panel runs in command mode over two DSI links, each carrying one
 * 1280x1600 half. Commands go to the DDI over the first link only.
 *
 * Power sequence, register writes and timings follow Samsung's SM-T860
 * kernel (ss_dsi_panel_ANA38401_AMSA05RB06 and its panel dtsi).
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

/* ANA38401 registers */
#define ANA38401_GPARA		0xb0	/* offset for the next 0xb2 write */
#define ANA38401_GPARA_DATA	0xb2
#define ANA38401_GAMMA		0x83	/* brightness condition (gamma offsets) */
#define ANA38401_AOR		0x90	/* AMOLED off ratio */

/* Global parameter offsets (in the 0xb2 bank) */
#define ANA38401_GP_UPDATE	0x35	/* write 0x01 to latch new settings */
#define ANA38401_GP_ACL		0x36
#define ANA38401_GP_ACL_START	0x49
#define ANA38401_GP_ACL_FRAMES	0x4d
#define ANA38401_GP_ELVSS	0x67
#define ANA38401_GP_TSP_SYNC	0x7f
#define ANA38401_GP_GAMMA_IDX	0xbc

/*
 * Brightness is set with the AMOLED off ratio, at the gamma the panel is
 * calibrated for at 360 nits. The AOR counts in units of 4 of the 1600
 * lines, so the emission duty is (400 - AOR) / 400. Samsung's own tables
 * use AOR 0x00e at 360 nits and at most 0x18d (2 nits); stay within that.
 */
#define AMSA05RB06_AOR_FRAME	400
#define AMSA05RB06_AOR_MIN	0x00e
#define AMSA05RB06_AOR_MAX	0x18d
#define AMSA05RB06_MAX_NITS	360
#define AMSA05RB06_MIN_NITS	2

/* Display On must not be sent earlier than this after reset (TCON_RDY) */
#define AMSA05RB06_DISPLAY_ON_DELAY_MS	220

/*
 * Bring-up switches, each testing one way this driver differs from the
 * stock one while the panel takes frames but shows no correct picture.
 */
static bool handover;
module_param(handover, bool, 0444);
MODULE_PARM_DESC(handover,
		 "Keep the panel as the bootloader left it at the first power-on: no reset, no commands");

static bool clock_noncontinuous;
module_param(clock_noncontinuous, bool, 0444);
MODULE_PARM_DESC(clock_noncontinuous,
		 "Let the DSI clock lane rest in LP-11 between transfers, so the panel is reset and waits for TCON_RDY before the HS clock starts, as in the Anapass power sequence");

static bool stock_init;
module_param(stock_init, bool, 0444);
MODULE_PARM_DESC(stock_init,
		 "Initialise the panel with the stock command list byte for byte: AOR for 360 nits, no tear effect command");

/*
 * Reading the panel goes through a bus turnaround, which the MSM DSI host
 * does not keep clear of command mode frames, and the first modeset runs
 * inside this driver's probe (from mipi_dsi_attach()). So read once right
 * after Display On, before the first frame, and the rest only seconds
 * later. Off by default: a read that went wrong there once kept the probe
 * from finishing, and storage and USB from probing after it.
 */
static bool status_log;
module_param(status_log, bool, 0444);
MODULE_PARM_DESC(status_log,
		 "Log the panel's own view (power mode, DSI errors, error flag) for 30 s after it is switched on");

/* When to read the panel's status again, in ms after Display On */
static const unsigned int amsa05rb06_status_ms[] = {
	5000, 10000, 20000, 30000,
};

struct amsa05rb06 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *tcon_rdy_gpio;
	struct gpio_desc *err_fg_gpio;
	struct delayed_work status_work;
	unsigned int status_step;
	ktime_t display_on;
	ktime_t reset_released;
	bool prepared;
	/* handover: on as the bootloader left it, so send it nothing */
	bool handed_over;
};

static const struct regulator_bulk_data amsa05rb06_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vdd" },
};

static inline struct amsa05rb06 *to_amsa05rb06(struct drm_panel *panel)
{
	return container_of(panel, struct amsa05rb06, panel);
}

/*
 * mipi_dsi_dcs_write_buffer() sends a two byte write as a DCS short write.
 * The DDI takes global parameter data (0xb2) as a DCS long write only:
 * Samsung's command tables send "b0 <offset>" short and "b2 <value>" long,
 * every time. Send it the same way.
 */
static void ana38401_dcs_write_long(struct mipi_dsi_multi_context *dsi_ctx,
				    const u8 *data, size_t len)
{
	struct mipi_dsi_device *dsi = dsi_ctx->dsi;
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_DCS_LONG_WRITE,
		.tx_buf = data,
		.tx_len = len,
	};
	ssize_t ret;

	if (dsi_ctx->accum_err)
		return;

	if (dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg.flags |= MIPI_DSI_MSG_USE_LPM;

	ret = dsi->host->ops->transfer(dsi->host, &msg);
	if (ret < 0) {
		dev_err(&dsi->dev, "sending dcs data %*ph failed: %zd\n",
			(int)len, data, ret);
		dsi_ctx->accum_err = ret;
	}
}

static void ana38401_gpara_write(struct mipi_dsi_multi_context *dsi_ctx,
				 u8 offset, u8 value)
{
	const u8 sel[] = { ANA38401_GPARA, offset };
	const u8 data[] = { ANA38401_GPARA_DATA, value };

	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, sel, sizeof(sel));
	ana38401_dcs_write_long(dsi_ctx, data, sizeof(data));
}

static u16 amsa05rb06_nits_to_aor(unsigned int nits)
{
	unsigned int aor;

	nits = clamp(nits, AMSA05RB06_MIN_NITS, AMSA05RB06_MAX_NITS);
	aor = AMSA05RB06_AOR_FRAME -
	      DIV_ROUND_CLOSEST(nits * (AMSA05RB06_AOR_FRAME - AMSA05RB06_AOR_MIN),
				AMSA05RB06_MAX_NITS);

	return clamp(aor, AMSA05RB06_AOR_MIN, AMSA05RB06_AOR_MAX);
}

static void amsa05rb06_write_aor(struct mipi_dsi_multi_context *dsi_ctx,
				 unsigned int nits)
{
	u16 aor = amsa05rb06_nits_to_aor(nits);
	const u8 cmd[] = { ANA38401_AOR, aor >> 8, 0x00, aor & 0xff };

	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, cmd, sizeof(cmd));
}

static void amsa05rb06_set_aor(struct mipi_dsi_multi_context *dsi_ctx,
			       unsigned int nits)
{
	amsa05rb06_write_aor(dsi_ctx, nits);
	ana38401_gpara_write(dsi_ctx, ANA38401_GP_UPDATE, 0x01);
}

static void amsa05rb06_reset(struct amsa05rb06 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(70);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	ctx->reset_released = ktime_get();
}

static int amsa05rb06_wait_tcon_ready(struct amsa05rb06 *ctx)
{
	struct device *dev = &ctx->dsi[0]->dev;
	int ret, val;

	if (!ctx->tcon_rdy_gpio)
		return 0;

	ret = read_poll_timeout(gpiod_get_value_cansleep, val, val != 0,
				1000, 200000, false, ctx->tcon_rdy_gpio);
	if (ret)
		dev_warn(dev, "TCON_RDY did not go high, continuing\n");
	else
		dev_dbg(dev, "TCON_RDY high %lld ms after reset\n",
			ktime_ms_delta(ktime_get(), ctx->reset_released));

	return 0;
}

static int amsa05rb06_on(struct amsa05rb06 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };
	/* Neutral gamma offsets: the panel's 360-nit calibration */
	static const u8 gamma[] = {
		ANA38401_GAMMA,
		0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00,
		0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00,
		0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00,
	};

	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_TSP_SYNC, 0x15);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_GAMMA_IDX, 0x04);
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, gamma, sizeof(gamma));
	if (stock_init)
		amsa05rb06_write_aor(&dsi_ctx, AMSA05RB06_MAX_NITS);
	else
		amsa05rb06_set_aor(&dsi_ctx,
				   backlight_get_brightness(ctx->panel.backlight));
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ELVSS, 0x0f);
	/* ACL off: 16 frame average, 60% start */
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ACL_FRAMES, 0x04);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ACL_START, 0x99);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ACL, 0x10);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_UPDATE, 0x01);
	if (!stock_init)
		mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx,
					       MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	return dsi_ctx.accum_err;
}

static int amsa05rb06_read_reg(struct mipi_dsi_device *dsi, u8 cmd)
{
	u8 val;
	ssize_t ret;

	ret = mipi_dsi_dcs_read(dsi, cmd, &val, 1);
	if (ret < 0)
		return ret;

	return ret == 1 ? val : -EIO;
}

/*
 * Bring-up: what the panel itself says, while it takes frames but shows
 * no correct picture. Power mode bits: 7 booster on, 4 sleep out,
 * 3 normal mode, 2 display on. Signal mode bit 7: TE output on. The DSI
 * error count is of packets the panel received with ECC or CRC errors
 * (bit 7: overflow). The error flag is the panel's ESD detect output.
 */
static void amsa05rb06_status_log(struct amsa05rb06 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi[0];

	dev_info(&dsi->dev,
		 "status at %lld ms: power mode %d, signal mode %d, DSI errors %d, diagnostic %d, TCON_RDY %d, error flag %d\n",
		 ktime_ms_delta(ktime_get(), ctx->display_on),
		 amsa05rb06_read_reg(dsi, MIPI_DCS_GET_POWER_MODE),
		 amsa05rb06_read_reg(dsi, MIPI_DCS_GET_SIGNAL_MODE),
		 amsa05rb06_read_reg(dsi, MIPI_DCS_GET_ERROR_COUNT_ON_DSI),
		 amsa05rb06_read_reg(dsi, MIPI_DCS_GET_DIAGNOSTIC_RESULT),
		 ctx->tcon_rdy_gpio ?
			gpiod_get_value_cansleep(ctx->tcon_rdy_gpio) : -ENOENT,
		 ctx->err_fg_gpio ?
			gpiod_get_value_cansleep(ctx->err_fg_gpio) : -ENOENT);
}

static void amsa05rb06_status_work(struct work_struct *work)
{
	struct amsa05rb06 *ctx = container_of(to_delayed_work(work),
					      struct amsa05rb06, status_work);
	unsigned int next;
	s64 now;

	amsa05rb06_status_log(ctx);

	if (++ctx->status_step >= ARRAY_SIZE(amsa05rb06_status_ms))
		return;

	next = amsa05rb06_status_ms[ctx->status_step];
	now = ktime_ms_delta(ktime_get(), ctx->display_on);
	schedule_delayed_work(&ctx->status_work,
			      msecs_to_jiffies(next > now ? next - now : 0));
}

/* Called from .enable, right after Display On and before the first frame */
static void amsa05rb06_status_start(struct amsa05rb06 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi[0];

	if (!status_log)
		return;

	ctx->display_on = ktime_get();
	ctx->status_step = 0;

	dev_info(&dsi->dev, "status: IDs %d %d %d\n",
		 amsa05rb06_read_reg(dsi, 0xda),
		 amsa05rb06_read_reg(dsi, 0xdb),
		 amsa05rb06_read_reg(dsi, 0xdc));
	amsa05rb06_status_log(ctx);

	schedule_delayed_work(&ctx->status_work,
			      msecs_to_jiffies(amsa05rb06_status_ms[0]));
}

static int amsa05rb06_prepare(struct drm_panel *panel)
{
	struct amsa05rb06 *ctx = to_amsa05rb06(panel);
	struct device *dev = &ctx->dsi[0]->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(amsa05rb06_supplies),
				    ctx->supplies);
	if (ret < 0)
		return ret;

	if (ctx->handed_over) {
		ctx->prepared = true;
		dev_info(dev, "handover: keeping the panel as the bootloader left it\n");
		return 0;
	}

	msleep(20);

	/* Anapass power sequence: VDD on, LP-11, reset, TCON_RDY, HS */
	amsa05rb06_reset(ctx);
	amsa05rb06_wait_tcon_ready(ctx);

	ret = amsa05rb06_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(amsa05rb06_supplies),
				       ctx->supplies);
		return ret;
	}

	ctx->prepared = true;
	dev_info(dev, "panel initialised\n");

	return 0;
}

static int amsa05rb06_enable(struct drm_panel *panel)
{
	struct amsa05rb06 *ctx = to_amsa05rb06(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };
	s64 elapsed = ktime_ms_delta(ktime_get(), ctx->reset_released);

	/* Already on */
	if (ctx->handed_over) {
		amsa05rb06_status_start(ctx);
		return 0;
	}

	if (elapsed < AMSA05RB06_DISPLAY_ON_DELAY_MS)
		msleep(AMSA05RB06_DISPLAY_ON_DELAY_MS - elapsed);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	if (!dsi_ctx.accum_err) {
		dev_info(&ctx->dsi[0]->dev, "display on\n");
		amsa05rb06_status_start(ctx);
	}

	return dsi_ctx.accum_err;
}

static int amsa05rb06_disable(struct drm_panel *panel)
{
	struct amsa05rb06 *ctx = to_amsa05rb06(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };

	cancel_delayed_work_sync(&ctx->status_work);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	return dsi_ctx.accum_err;
}

static int amsa05rb06_unprepare(struct drm_panel *panel)
{
	struct amsa05rb06 *ctx = to_amsa05rb06(panel);

	ctx->prepared = false;
	ctx->handed_over = false;
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	regulator_bulk_disable(ARRAY_SIZE(amsa05rb06_supplies), ctx->supplies);

	return 0;
}

/*
 * Two 1280x1600 halves. The bonded DSI host halves the horizontal timings,
 * so per link this is the stock 1280 + 110 + 42 + 110 at 898 Mbps/lane.
 */
static const struct drm_display_mode amsa05rb06_mode = {
	.clock = (2560 + 220 + 84 + 220) * (1600 + 10 + 2 + 6) * 60 / 1000,
	.hdisplay = 2560,
	.hsync_start = 2560 + 220,
	.hsync_end = 2560 + 220 + 84,
	.htotal = 2560 + 220 + 84 + 220,
	.vdisplay = 1600,
	.vsync_start = 1600 + 10,
	.vsync_end = 1600 + 10 + 2,
	.vtotal = 1600 + 10 + 2 + 6,
	.width_mm = 227,
	.height_mm = 142,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int amsa05rb06_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &amsa05rb06_mode);
}

static const struct drm_panel_funcs amsa05rb06_panel_funcs = {
	.prepare = amsa05rb06_prepare,
	.enable = amsa05rb06_enable,
	.disable = amsa05rb06_disable,
	.unprepare = amsa05rb06_unprepare,
	.get_modes = amsa05rb06_get_modes,
};

static int amsa05rb06_bl_update_status(struct backlight_device *bl)
{
	struct amsa05rb06 *ctx = bl_get_data(bl);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };

	/* Applied by amsa05rb06_on() when the panel is next powered */
	if (!ctx->prepared || ctx->handed_over)
		return 0;

	amsa05rb06_set_aor(&dsi_ctx, backlight_get_brightness(bl));

	return dsi_ctx.accum_err;
}

static const struct backlight_ops amsa05rb06_bl_ops = {
	.update_status = amsa05rb06_bl_update_status,
};

static struct backlight_device *amsa05rb06_create_backlight(struct amsa05rb06 *ctx)
{
	struct device *dev = &ctx->dsi[0]->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.scale = BACKLIGHT_SCALE_LINEAR,
		.brightness = 180,
		.max_brightness = AMSA05RB06_MAX_NITS,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &amsa05rb06_bl_ops, &props);
}

static int amsa05rb06_probe(struct mipi_dsi_device *dsi)
{
	static const struct mipi_dsi_device_info dsi1_info = {
		.type = "AMSA05RB06 DSI1",
	};
	struct device *dev = &dsi->dev;
	struct device_node *dsi1_node;
	struct mipi_dsi_host *dsi1_host;
	struct amsa05rb06 *ctx;
	int ret, i;

	ctx = devm_drm_panel_alloc(dev, struct amsa05rb06, panel,
				   &amsa05rb06_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(amsa05rb06_supplies),
					    amsa05rb06_supplies,
					    &ctx->supplies);
	if (ret < 0)
		return ret;

	/* For a handover, leave the panel out of reset, as the bootloader did */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset",
					 handover ? GPIOD_OUT_LOW : GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->tcon_rdy_gpio = devm_gpiod_get_optional(dev, "tcon-rdy", GPIOD_IN);
	if (IS_ERR(ctx->tcon_rdy_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->tcon_rdy_gpio),
				     "Failed to get tcon-rdy-gpios\n");

	ctx->err_fg_gpio = devm_gpiod_get_optional(dev, "err-fg", GPIOD_IN);
	if (IS_ERR(ctx->err_fg_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->err_fg_gpio),
				     "Failed to get err-fg-gpios\n");

	INIT_DELAYED_WORK(&ctx->status_work, amsa05rb06_status_work);

	dsi1_node = of_graph_get_remote_node(dev->of_node, 1, -1);
	if (!dsi1_node)
		return dev_err_probe(dev, -ENODEV,
				     "Missing port@1 (second DSI link)\n");

	dsi1_host = of_find_mipi_dsi_host_by_node(dsi1_node);
	of_node_put(dsi1_node);
	if (!dsi1_host)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "Cannot get secondary DSI host\n");

	ctx->dsi[1] = devm_mipi_dsi_device_register_full(dev, dsi1_host,
							 &dsi1_info);
	if (IS_ERR(ctx->dsi[1]))
		return dev_err_probe(dev, PTR_ERR(ctx->dsi[1]),
				     "Cannot register secondary DSI device\n");

	ctx->dsi[0] = dsi;
	ctx->handed_over = handover;
	mipi_dsi_set_drvdata(dsi, ctx);
	mipi_dsi_set_drvdata(ctx->dsi[1], ctx);

	/* Initialize the panel with the DSI link up (in LP-11) */
	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = amsa05rb06_create_backlight(ctx);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ctx->dsi[i]->lanes = 4;
		ctx->dsi[i]->format = MIPI_DSI_FMT_RGB888;
		/* Command mode, continuous HS clock, commands in HS */
		ctx->dsi[i]->mode_flags = 0;
		if (clock_noncontinuous)
			ctx->dsi[i]->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;

		ret = devm_mipi_dsi_attach(dev, ctx->dsi[i]);
		if (ret < 0) {
			drm_panel_remove(&ctx->panel);
			return dev_err_probe(dev, ret,
					     "Failed to attach to DSI%d\n", i);
		}
	}

	return 0;
}

static void amsa05rb06_remove(struct mipi_dsi_device *dsi)
{
	struct amsa05rb06 *ctx = mipi_dsi_get_drvdata(dsi);

	cancel_delayed_work_sync(&ctx->status_work);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id amsa05rb06_of_match[] = {
	{ .compatible = "samsung,amsa05rb06" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amsa05rb06_of_match);

static struct mipi_dsi_driver amsa05rb06_driver = {
	.probe = amsa05rb06_probe,
	.remove = amsa05rb06_remove,
	.driver = {
		.name = "panel-samsung-amsa05rb06",
		.of_match_table = amsa05rb06_of_match,
	},
};
module_mipi_dsi_driver(amsa05rb06_driver);

MODULE_DESCRIPTION("DRM driver for the Samsung AMSA05RB06 (ANA38401) AMOLED panel");
MODULE_LICENSE("GPL");
