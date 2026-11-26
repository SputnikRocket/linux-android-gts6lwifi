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
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include "panel-samsung-amsa05rb06-dimming.h"

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
 * calibrated for at 360 nits. The AOR counts in units of 4 lines, and
 * Samsung's interpolation counts the frame as its 1600 lines and 16 lines
 * of porches, so the emission duty is (404 - AOR) / 404. Samsung's own
 * tables use AOR 0x00e at 360 nits and at most 0x18d (2 nits); stay within
 * that.
 */
#define AMSA05RB06_AOR_FRAME	404
#define AMSA05RB06_AOR_MIN	0x00e
#define AMSA05RB06_AOR_MAX	0x18d
#define AMSA05RB06_MAX_NITS	360
#define AMSA05RB06_MIN_NITS	2
/*
 * The backlight device counts in tenths of a nit above the panel's 2 nit
 * minimum, so 0 is the dimmest the panel goes, not off (as Android's
 * brightness 0.0). In whole nits from 0, a step is 7% at 15 nits, where
 * the AOR can do three times finer, and the bottom of Android's slider
 * (its first eighth gives under 2 nits) is dead.
 */
#define AMSA05RB06_BL_SCALE	10
#define AMSA05RB06_BL_MAX	((AMSA05RB06_MAX_NITS - AMSA05RB06_MIN_NITS) * \
				 AMSA05RB06_BL_SCALE)

/*
 * The gamma register: per colour, V11, V23, V35, V51, V87, V151, V203,
 * V255[7:0], V3, V255[8], VT. The panel's factory calibration (MTP) has
 * the same layout.
 */
#define ANA38401_GAMMA_LEN	ANA38401_DIMMING_GAMMA_LEN
#define ANA38401_MTP		0xd8	/* at offset 0x5a */
#define ANA38401_MTP_OFFSET	0x5a

/* Samsung's placeholder in its init commands: uncalibrated */
static const u8 ana38401_gamma_nominal[ANA38401_GAMMA_LEN] = {
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00,
};

/*
 * Samsung's init commands carry a placeholder gamma, which its driver
 * replaces right after with one from the panel's calibration (with the
 * gamma offset index at 4, the DDI does not add the calibration itself).
 * With the placeholder, this panel shows its dark greys far too dark.
 */
static bool mtp_gamma = true;
module_param(mtp_gamma, bool, 0644);
MODULE_PARM_DESC(mtp_gamma,
		 "Use the gamma from the panel's factory calibration (default), not the uncalibrated placeholder. Applied each time the panel is switched on");

/*
 * Samsung's smart dimming: each of its 65 brightness levels has a gamma
 * computed from the calibration (for gamma 2.15 below 360 nits, with
 * corrections for each level), and its own AOR and ELVSS. Without it, the
 * 360-nit gamma is dimmed with the AOR alone, which turns the greys red at
 * mid brightness and black at the bottom.
 */
static bool smart_dimming = true;
module_param(smart_dimming, bool, 0644);
MODULE_PARM_DESC(smart_dimming,
		 "Set each brightness level's own gamma, AOR and ELVSS, and dim between levels with the AOR, as Samsung's driver does (default), not the 360-nit gamma dimmed with the AOR. Needs mtp_gamma; applied at the next brightness change");

/* Reading the calibration after the first Display On, from a work item */
#define AMSA05RB06_CALIB_DELAY_MS	1000

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
 * The AOR command has a middle byte that Samsung's driver always sends as
 * 0 (the AOR read back from 0x89 is 3 bytes too). If it held finer bits of
 * the AOR, brightness could step finer than a unit, which below 20 nits is
 * a visible step: this sends another value, to look.
 */
static unsigned int aor_mid;
module_param(aor_mid, uint, 0644);
MODULE_PARM_DESC(aor_mid,
		 "Experimental: the middle byte of the AOR command (Samsung sends 0); applied at the next brightness change");

/*
 * Reading the panel goes through a bus turnaround, and the first modeset
 * runs inside this driver's probe (from mipi_dsi_attach()): reads there
 * have kept the probe from finishing, and storage and USB from probing
 * after it, more than once. So read only from a work item, seconds after
 * Display On. Off by default.
 */
static bool status_log;
module_param(status_log, bool, 0444);
MODULE_PARM_DESC(status_log,
		 "Log the panel's own view (power mode, DSI errors, error flag) for 30 s after it is switched on");

/*
 * The DDI's colour engine (Samsung's mDNIe), with Samsung's modes. auto is
 * Samsung's default ("Vivid"): the panel's native wide gamut, which shows
 * sRGB content too saturated, and on this tablet the same as the DDI's
 * own state after reset. natural maps it to sRGB, but also moves the white
 * point to a warmer one, which Samsung's driver then corrects for each
 * panel from its measured colour coordinates (not done here), so it looks
 * tinted. natural_cool is natural's sRGB gamut with the panel's own white
 * point, as in auto; on the tablet it looks darker than auto rather than
 * less saturated. srgb, the default, is the default mode of the stock
 * display calibration instead (Qualcomm's QDCM, in the stock vendor
 * partition), which maps sRGB onto the panel's gamut without darkening the
 * primaries as much, and leaves white and the greys alone.
 */
static char *screen_mode = "srgb";
module_param(screen_mode, charp, 0644);
MODULE_PARM_DESC(screen_mode,
		 "Colour mode: srgb (the stock calibration's sRGB mode; default); Samsung's for the UI, auto (Samsung's default, \"Vivid\"), natural (sRGB), standard, dynamic, movie; natural_cool (natural's gamut, the panel's white); or off (the DDI's own state after reset). Applied each time the panel is switched on");

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
	struct delayed_work calib_work;
	/* Command sequences (0xb0 offset, then a write or read) stay whole */
	struct mutex cmd_lock;
	/* The gamma for 360 nits, from the calibration */
	u8 gamma[ANA38401_GAMMA_LEN];
	bool gamma_calibrated;
	/* ... and for each brightness level (ana38401_levels) */
	u8 level_gamma[ANA38401_DIMMING_LEVELS][ANA38401_GAMMA_LEN];
	bool level_gamma_ok;
	ktime_t display_on;
	ktime_t reset_released;
	bool prepared;
	bool calibration_logged;
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

/* Brightness in tenths of a nit, from the backlight device's value */
static unsigned int amsa05rb06_bl_to_dnits(unsigned int bl)
{
	return AMSA05RB06_MIN_NITS * AMSA05RB06_BL_SCALE +
	       min(bl, AMSA05RB06_BL_MAX);
}

/* The AOR for a brightness (in tenths of a nit) at the 360 nit gamma */
static u16 amsa05rb06_dnits_to_aor(unsigned int dnits)
{
	unsigned int aor;

	aor = AMSA05RB06_AOR_FRAME -
	      DIV_ROUND_CLOSEST(dnits * (AMSA05RB06_AOR_FRAME - AMSA05RB06_AOR_MIN),
				AMSA05RB06_MAX_NITS * AMSA05RB06_BL_SCALE);

	return clamp(aor, AMSA05RB06_AOR_MIN, AMSA05RB06_AOR_MAX);
}

static void amsa05rb06_write_aor(struct mipi_dsi_multi_context *dsi_ctx,
				 u16 aor)
{
	const u8 cmd[] = { ANA38401_AOR, aor >> 8, aor_mid & 0xff, aor & 0xff };

	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, cmd, sizeof(cmd));
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

/*
 * Samsung's mDNIe tables for the UI, from its SM-T860 kernel
 * (ss_dsi_mdnie_ANA38401_AMSA05RB06.h, Copyright (C) 2017 Samsung
 * Electronics, GPL-2.0). Each is two 0xb9 writes: the gamut mapping
 * (SCR) at global parameter offset 0xa8, then the enable bits, curves
 * and the rest at offset 0x42.
 */
static const u8 ana38401_mdnie_dynamic_1[] = {
	0xb9, 0x20, 0x67, 0xa9, 0x37, 0x29, 0x19, 0x47, 0x00, 0x25, 0x3d, 0x00,
	0x31, 0xf4, 0x00, 0x51, 0xec, 0x00, 0x1c, 0xd8, 0xff, 0x60, 0x82, 0xff,
	0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xf4, 0xff, 0x66, 0xdf, 0xfc, 0x00,
	0xf3, 0x00, 0xf2, 0x54, 0x00, 0xff, 0xf4, 0x00, 0xf5, 0x2f, 0xf1, 0x32,
	0x00, 0xea, 0xff, 0x00, 0xfc, 0x00, 0xf6, 0x00,
};

static const u8 ana38401_mdnie_dynamic_2[] = {
	0xb9, 0x01, 0x00, 0x3c, 0x98, 0x24, 0x00, 0xb3, 0x01, 0x0e, 0x01, 0x00,
	0x66, 0x17, 0x03, 0x96, 0x07, 0xff, 0x00, 0x10, 0x02, 0x00, 0x02, 0x00,
	0x14, 0x00, 0x0a, 0x00, 0x32, 0x01, 0xf4, 0x0b, 0x8a, 0x20, 0x2d, 0x6e,
	0x99, 0x1b, 0x17, 0x14, 0x1e, 0x02, 0x5f, 0x02, 0xc8, 0x03, 0x33, 0x02,
	0x22, 0x10, 0x10, 0x07, 0x07, 0x01, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0xff,
};

static const u8 ana38401_mdnie_standard_1[] = {
	0xb9, 0x20, 0x67, 0xa9, 0x17, 0x29, 0x19, 0x27, 0x00, 0x59, 0x0b, 0x00,
	0x31, 0xf4, 0x00, 0x51, 0xec, 0x00, 0x34, 0x83, 0xff, 0x50, 0x60, 0xff,
	0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0xee, 0xec, 0x28,
	0xe6, 0x2e, 0xfd, 0x00, 0x4a, 0xda, 0xe9, 0x10, 0xee, 0x2c, 0xea, 0x30,
	0x44, 0xd4, 0xff, 0x00, 0xf7, 0x00, 0xef, 0x00,
};

static const u8 ana38401_mdnie_standard_2[] = {
	0xb9, 0x01, 0x00, 0x3c, 0x98, 0x24, 0x00, 0xb3, 0x01, 0x0e, 0x01, 0x00,
	0x66, 0x17, 0x03, 0x96, 0x00, 0xff, 0x00, 0x00, 0x07, 0xff, 0x07, 0xff,
	0x14, 0x00, 0x0a, 0x00, 0x32, 0x01, 0xf4, 0x0b, 0x8a, 0x20, 0x2d, 0x6e,
	0x99, 0x1b, 0x17, 0x14, 0x1e, 0x02, 0x5f, 0x02, 0xc8, 0x03, 0x33, 0x02,
	0x22, 0x10, 0x10, 0x07, 0x07, 0x01, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x09,
	0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0xff,
};

static const u8 ana38401_mdnie_natural_1[] = {
	0xb9, 0x20, 0x67, 0xa9, 0x17, 0x29, 0x19, 0x27, 0x00, 0x59, 0x0b, 0x00,
	0x31, 0xf4, 0x00, 0x51, 0xec, 0x00, 0x34, 0x83, 0xff, 0x50, 0x60, 0xff,
	0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0x8c, 0xc2, 0xe8, 0x1c,
	0xe2, 0x24, 0xd8, 0x88, 0x3f, 0xda, 0xda, 0x46, 0xe4, 0x26, 0xde, 0x2e,
	0x4a, 0xc8, 0xff, 0x00, 0xf7, 0x00, 0xef, 0x00,
};

static const u8 ana38401_mdnie_natural_2[] = {
	0xb9, 0x01, 0x00, 0x3c, 0x98, 0x24, 0x00, 0xb3, 0x01, 0x0e, 0x01, 0x00,
	0x66, 0x17, 0x03, 0x96, 0x00, 0xff, 0x00, 0x00, 0x07, 0xff, 0x07, 0xff,
	0x14, 0x00, 0x0a, 0x00, 0x32, 0x01, 0xf4, 0x0b, 0x8a, 0x20, 0x2d, 0x6e,
	0x99, 0x1b, 0x17, 0x14, 0x1e, 0x02, 0x5f, 0x02, 0xc8, 0x03, 0x33, 0x02,
	0x22, 0x10, 0x10, 0x07, 0x07, 0x01, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x09,
	0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0xff,
};

static const u8 ana38401_mdnie_movie_1[] = {
	0xb9, 0x00, 0x67, 0xa9, 0x0c, 0x0c, 0x0c, 0x0c, 0x00, 0xaa, 0xab, 0x00,
	0xaa, 0xab, 0x00, 0xaa, 0xab, 0x00, 0xaa, 0xab, 0xd5, 0x2c, 0x2a, 0xff,
	0xf5, 0x63, 0xfe, 0x4a, 0xff, 0xff, 0xf9, 0xf8, 0x00, 0xff, 0xff, 0x00,
	0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00,
	0x00, 0xff, 0xff, 0x00, 0xf9, 0x00, 0xe9, 0x00,
};

static const u8 ana38401_mdnie_movie_2[] = {
	0xb9, 0x01, 0x00, 0x00, 0x98, 0x24, 0x00, 0xb3, 0x01, 0x0e, 0x01, 0x00,
	0x66, 0x17, 0x03, 0x96, 0x00, 0xff, 0x00, 0x00, 0x07, 0xff, 0x07, 0xff,
	0x14, 0x00, 0x0a, 0x00, 0x32, 0x01, 0xf4, 0x0b, 0x8a, 0x20, 0x2d, 0x6e,
	0x99, 0x1b, 0x17, 0x14, 0x1e, 0x02, 0x5f, 0x02, 0xc8, 0x03, 0x33, 0x02,
	0x22, 0x10, 0x10, 0x07, 0x07, 0x01, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x09,
	0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0xff,
};

static const u8 ana38401_mdnie_auto_1[] = {
	0xb9, 0x00, 0x67, 0xa9, 0x17, 0x29, 0x19, 0x27, 0x00, 0x59, 0x0b, 0x00,
	0x31, 0xf4, 0x00, 0x51, 0xec, 0x00, 0x34, 0x83, 0xff, 0x5c, 0x68, 0xff,
	0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xf8, 0xff, 0x00, 0xff, 0xf2, 0x00,
	0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00,
	0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00,
};

static const u8 ana38401_mdnie_auto_2[] = {
	0xb9, 0x01, 0x00, 0x3c, 0x98, 0x24, 0x00, 0xb3, 0x01, 0x0e, 0x01, 0x00,
	0x66, 0x17, 0x03, 0x96, 0x00, 0xff, 0x00, 0x15, 0x00, 0xa0, 0x00, 0xa0,
	0x14, 0x00, 0x01, 0x00, 0x01, 0x00, 0x70, 0x01, 0xc0, 0x28, 0x3c, 0xa9,
	0x67, 0x27, 0x19, 0x29, 0x17, 0x01, 0xa4, 0x02, 0x8f, 0x01, 0x90, 0x02,
	0xc8, 0x20, 0x20, 0x07, 0x07, 0x01, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x02, 0x1b, 0x09,
	0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x09, 0xa6, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
	0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0xff,
};

/*
 * The gamut mapping (SCR) in the first part: the colour each corner of the
 * RGB cube is shown as, as pairs of red, green and blue values for cyan and
 * red, magenta and green, yellow and blue, then white and black (ascr_Cr,
 * Rr, Cg, Rg, Cb, Rb, Mr, Gr, ..., Wb, Kb). natural's first byte has
 * linear_on, for interpolating between the corners in linear light.
 */
#define ANA38401_MDNIE_SCR	32
#define ANA38401_MDNIE_SCR_LEN	24
#define ANA38401_MDNIE_WHITE	50

/*
 * The stock calibration's default (sRGB) mode, as SCR corners: its 3D LUT
 * (in linear light, between an sRGB decode and encode) applied to each
 * corner. Its grey axis is the identity.
 */
static const u8 ana38401_scr_qdcm_srgb[ANA38401_MDNIE_SCR_LEN] = {
	0x72, 0xef, 0xfa, 0x39, 0xfe, 0x28,	/* cyan, red */
	0xec, 0x72, 0x2e, 0xfe, 0xf9, 0x4b,	/* magenta, green */
	0xff, 0x10, 0xff, 0x00, 0x54, 0xf9,	/* yellow, blue */
	0xff, 0x00, 0xff, 0x00, 0xff, 0x00,	/* white, black */
};

static const struct ana38401_mdnie_mode {
	const char *name;
	const u8 *scr;
	size_t scr_len;
	const u8 *ctrl;
	size_t ctrl_len;
	bool panel_white;	/* keep the panel's white point (ff ff ff) */
	const u8 *scr_override;	/* other SCR corners */
} ana38401_mdnie_modes[] = {
	{ "srgb",
	  ana38401_mdnie_natural_1, sizeof(ana38401_mdnie_natural_1),
	  ana38401_mdnie_natural_2, sizeof(ana38401_mdnie_natural_2),
	  .scr_override = ana38401_scr_qdcm_srgb },
#define ANA38401_MDNIE_MODE(n, ...) { #n, \
	ana38401_mdnie_##n##_1, sizeof(ana38401_mdnie_##n##_1), \
	ana38401_mdnie_##n##_2, sizeof(ana38401_mdnie_##n##_2), __VA_ARGS__ }
	ANA38401_MDNIE_MODE(auto),
	{ "natural_cool",
	  ana38401_mdnie_natural_1, sizeof(ana38401_mdnie_natural_1),
	  ana38401_mdnie_natural_2, sizeof(ana38401_mdnie_natural_2), true },
	ANA38401_MDNIE_MODE(natural),
	ANA38401_MDNIE_MODE(standard),
	ANA38401_MDNIE_MODE(dynamic),
	ANA38401_MDNIE_MODE(movie),
#undef ANA38401_MDNIE_MODE
};

static_assert(ANA38401_MDNIE_SCR + ANA38401_MDNIE_SCR_LEN <=
	      sizeof(ana38401_mdnie_natural_1));

static void amsa05rb06_set_screen_mode(struct amsa05rb06 *ctx,
				       struct mipi_dsi_multi_context *dsi_ctx)
{
	static const u8 sel_scr[] = { ANA38401_GPARA, 0xa8 };
	static const u8 sel_ctrl[] = { ANA38401_GPARA, 0x42 };
	const struct ana38401_mdnie_mode *mode = &ana38401_mdnie_modes[0];
	u8 scr[sizeof(ana38401_mdnie_natural_1)];
	char name[16];
	int i;

	kernel_param_lock(THIS_MODULE);
	strscpy(name, screen_mode ?: "", sizeof(name));
	kernel_param_unlock(THIS_MODULE);

	if (sysfs_streq(name, "off"))
		return;

	for (i = 0; i < ARRAY_SIZE(ana38401_mdnie_modes); i++)
		if (sysfs_streq(name, ana38401_mdnie_modes[i].name))
			break;
	if (i < ARRAY_SIZE(ana38401_mdnie_modes))
		mode = &ana38401_mdnie_modes[i];
	else
		dev_warn(&ctx->dsi[0]->dev, "unknown screen_mode %s, using %s\n",
			 name, mode->name);

	memcpy(scr, mode->scr, min(mode->scr_len, sizeof(scr)));
	if (mode->scr_override)
		memcpy(&scr[ANA38401_MDNIE_SCR], mode->scr_override,
		       ANA38401_MDNIE_SCR_LEN);
	if (mode->panel_white)
		for (i = 0; i < 3; i++)
			scr[ANA38401_MDNIE_WHITE + 2 * i] = 0xff;

	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, sel_scr, sizeof(sel_scr));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, scr,
					min(mode->scr_len, sizeof(scr)));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, sel_ctrl, sizeof(sel_ctrl));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, mode->ctrl, mode->ctrl_len);
}

static void ana38401_write_gamma(struct mipi_dsi_multi_context *dsi_ctx,
				 const u8 *gamma)
{
	u8 cmd[1 + ANA38401_GAMMA_LEN] = { ANA38401_GAMMA };

	memcpy(&cmd[1], gamma, ANA38401_GAMMA_LEN);
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, cmd, sizeof(cmd));
}

/* The dimmest of Samsung's levels that is at least this bright */
static unsigned int ana38401_level_up(unsigned int nits)
{
	unsigned int i;

	for (i = 0; i < ANA38401_DIMMING_LEVELS - 1; i++)
		if (ana38401_levels[i].cd >= nits)
			break;

	return i;
}

/*
 * Gamma, AOR and ELVSS for a backlight value, latched together. They go in
 * the order of Samsung's brightness packet: AOR, ELVSS, gamma, update.
 */
static void amsa05rb06_set_brightness(struct amsa05rb06 *ctx,
				      struct mipi_dsi_multi_context *dsi_ctx,
				      unsigned int bl)
{
	bool calibrated = mtp_gamma && ctx->gamma_calibrated;
	unsigned int dnits = amsa05rb06_bl_to_dnits(bl);

	if (calibrated && smart_dimming && ctx->level_gamma_ok) {
		/*
		 * Between two levels, as Samsung's S dimming: the brighter
		 * level's gamma and ELVSS, and a longer AOR, which brings it
		 * down to the brightness asked for (light is emitted for
		 * AMSA05RB06_AOR_FRAME - AOR units of the frame). Rounded,
		 * that can come out below the dimmer level at its own
		 * settings; take one unit more then, so that brightness never
		 * goes down as the value goes up.
		 */
		unsigned int i = ana38401_level_up(DIV_ROUND_UP(dnits,
								AMSA05RB06_BL_SCALE));
		const struct ana38401_level *l = &ana38401_levels[i];
		unsigned int lit = AMSA05RB06_AOR_FRAME - l->aor;
		unsigned int on = DIV_ROUND_CLOSEST(lit * dnits,
						    l->cd * AMSA05RB06_BL_SCALE);

		if (i > 0)
			on = max(on, DIV_ROUND_UP(lit * ana38401_levels[i - 1].cd,
						  l->cd));

		amsa05rb06_write_aor(dsi_ctx, AMSA05RB06_AOR_FRAME - on);
		ana38401_gpara_write(dsi_ctx, ANA38401_GP_ELVSS, l->elvss);
		ana38401_write_gamma(dsi_ctx, ctx->level_gamma[i]);
	} else {
		amsa05rb06_write_aor(dsi_ctx, amsa05rb06_dnits_to_aor(dnits));
		ana38401_gpara_write(dsi_ctx, ANA38401_GP_ELVSS, 0x0f);
		ana38401_write_gamma(dsi_ctx, calibrated ? ctx->gamma :
				     ana38401_gamma_nominal);
	}
	ana38401_gpara_write(dsi_ctx, ANA38401_GP_UPDATE, 0x01);
}

static int amsa05rb06_on(struct amsa05rb06 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };

	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_TSP_SYNC, 0x15);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_GAMMA_IDX, 0x04);
	if (stock_init) {
		ana38401_write_gamma(&dsi_ctx, ana38401_gamma_nominal);
		amsa05rb06_write_aor(&dsi_ctx, AMSA05RB06_AOR_MIN);
		ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ELVSS, 0x0f);
	} else {
		amsa05rb06_set_brightness(ctx, &dsi_ctx,
					  backlight_get_brightness(ctx->panel.backlight));
	}
	/* ACL off: 16 frame average, 60% start */
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ACL_FRAMES, 0x04);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ACL_START, 0x99);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_ACL, 0x10);
	ana38401_gpara_write(&dsi_ctx, ANA38401_GP_UPDATE, 0x01);
	amsa05rb06_set_screen_mode(ctx, &dsi_ctx);
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
 * Reads of the DDI's calibration, as Samsung's driver does them: in parts
 * of at most 10 bytes, each with the offset into the register set first
 * (0xb0) and the length as the second parameter of a generic read.
 */
#define ANA38401_READ_MAX	10

static int ana38401_read(struct mipi_dsi_device *dsi, u8 reg, u8 offset,
			 u8 *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		u8 n = min_t(size_t, len - done, ANA38401_READ_MAX);
		const u8 pos[] = { ANA38401_GPARA, offset + done };
		const u8 params[] = { reg, n };
		ssize_t ret;

		ret = mipi_dsi_dcs_write_buffer(dsi, pos, sizeof(pos));
		if (ret < 0)
			return ret;

		ret = mipi_dsi_generic_read(dsi, params, sizeof(params),
					    buf + done, n);
		if (ret < 0)
			return ret;
		if (ret != n)
			return -EIO;

		done += n;
	}

	return 0;
}

/* The MTP's offsets are sign and magnitude */
static int ana38401_mtp_offset(u8 v)
{
	return v & 0x80 ? -(int)(v & 0x7f) : v;
}

/*
 * The gamma at 360 nits, from the panel's calibration, as Samsung's
 * gamma_cell_determine_max(): each grey point's value is 128 plus its
 * offset, and V255's is 256 plus its offset (V255[8] holds the sign of the
 * offset in the MTP, and bit 8 of the value in the gamma register).
 */
static void ana38401_gamma_from_mtp(const u8 *mtp, u8 *gamma)
{
	int c, i;

	for (c = 0; c < 3; c++) {
		const u8 *m = &mtp[c * 11];
		u8 *g = &gamma[c * 11];
		int v255 = 256 + (m[9] ? -m[7] : m[7]);

		for (i = 0; i < 7; i++)		/* V11 to V203 */
			g[i] = 128 + ana38401_mtp_offset(m[i]);
		g[7] = v255 & 0xff;
		g[8] = 128 + ana38401_mtp_offset(m[8]);	/* V3 */
		g[9] = v255 >= 256 ? 0x80 : 0;
		g[10] = 0;				/* VT */
	}
}

/*
 * Read the calibration once, after the first Display On: the first
 * power-on runs inside this driver's probe, where reads are not safe (see
 * status_log). Then apply its gamma right away, and at every power-on.
 */
static void amsa05rb06_calib_work(struct work_struct *work)
{
	struct amsa05rb06 *ctx = container_of(to_delayed_work(work),
					      struct amsa05rb06, calib_work);
	struct mipi_dsi_device *dsi = ctx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	u8 mtp[ANA38401_GAMMA_LEN];
	int ret;

	mutex_lock(&ctx->cmd_lock);
	if (!ctx->prepared || ctx->gamma_calibrated)
		goto out;

	ret = ana38401_read(dsi, ANA38401_MTP, ANA38401_MTP_OFFSET, mtp,
			    sizeof(mtp));
	if (ret) {
		dev_warn(&dsi->dev, "cannot read the gamma calibration: %d\n",
			 ret);
		goto out;
	}
	if (!memchr_inv(mtp, 0, sizeof(mtp)) ||
	    !memchr_inv(mtp, 0xff, sizeof(mtp))) {
		dev_warn(&dsi->dev, "no gamma calibration in the panel\n");
		goto out;
	}

	ana38401_gamma_from_mtp(mtp, ctx->gamma);
	ctx->gamma_calibrated = true;
	dev_info(&dsi->dev, "gamma from the panel's calibration: %*ph\n",
		 ANA38401_GAMMA_LEN, ctx->gamma);

	ret = ana38401_dimming_gamma(mtp, ctx->level_gamma);
	if (ret)
		dev_warn(&dsi->dev, "no smart dimming: %d\n", ret);
	else if (memcmp(ctx->level_gamma[ANA38401_DIMMING_LEVELS - 1],
			ctx->gamma, ANA38401_GAMMA_LEN))
		dev_warn(&dsi->dev, "no smart dimming: its 360-nit gamma differs\n");
	else
		ctx->level_gamma_ok = true;
	if (ctx->level_gamma_ok)
		dev_info(&dsi->dev, "smart dimming: %u levels from the panel's calibration\n",
			 ANA38401_DIMMING_LEVELS);

	amsa05rb06_set_brightness(ctx, &dsi_ctx,
				  backlight_get_brightness(ctx->panel.backlight));
out:
	mutex_unlock(&ctx->cmd_lock);
}

/*
 * Bring-up: the panel's own calibration, which Samsung's driver reads at
 * the first power-on: the gamma MTP and HBM gamma (for its smart dimming),
 * the white point's measured colour coordinates (for its mDNIe white
 * point correction), the ELVSS setting, and the manufacture date and cell
 * ID. Logged once, with status_log.
 */
static void amsa05rb06_log_calibration(struct amsa05rb06 *ctx)
{
	static const struct {
		const char *name;
		u8 reg, offset, len;
	} reads[] = {
		{ "gamma MTP", 0xd8, 0x5a, 33 },
		{ "HBM gamma", 0xd4, 0x5b, 33 },
		{ "white coordinates", 0xd8, 0x7b, 4 },
		{ "ELVSS", 0xd2, 0x70, 1 },
		{ "date and cell ID", 0xd8, 0x86, 7 },
	};
	struct mipi_dsi_device *dsi = ctx->dsi[0];
	u8 buf[33];
	int i, ret;

	if (ctx->calibration_logged)
		return;
	ctx->calibration_logged = true;

	for (i = 0; i < ARRAY_SIZE(reads); i++) {
		ret = ana38401_read(dsi, reads[i].reg, reads[i].offset, buf,
				    reads[i].len);
		if (ret)
			dev_info(&dsi->dev, "calibration: %s: read failed: %d\n",
				 reads[i].name, ret);
		else
			dev_info(&dsi->dev, "calibration: %s: %*ph\n",
				 reads[i].name, reads[i].len, buf);
	}
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

	mutex_lock(&ctx->cmd_lock);
	if (ctx->status_step == 0) {
		struct mipi_dsi_device *dsi = ctx->dsi[0];

		dev_info(&dsi->dev, "status: IDs %d %d %d\n",
			 amsa05rb06_read_reg(dsi, 0xda),
			 amsa05rb06_read_reg(dsi, 0xdb),
			 amsa05rb06_read_reg(dsi, 0xdc));
		amsa05rb06_log_calibration(ctx);
	}
	amsa05rb06_status_log(ctx);
	mutex_unlock(&ctx->cmd_lock);

	if (++ctx->status_step >= ARRAY_SIZE(amsa05rb06_status_ms))
		return;

	next = amsa05rb06_status_ms[ctx->status_step];
	now = ktime_ms_delta(ktime_get(), ctx->display_on);
	schedule_delayed_work(&ctx->status_work,
			      msecs_to_jiffies(next > now ? next - now : 0));
}

/* Called from .enable, right after Display On: no reads here (see above) */
static void amsa05rb06_status_start(struct amsa05rb06 *ctx)
{
	if (!status_log)
		return;

	ctx->display_on = ktime_get();
	ctx->status_step = 0;

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

	mutex_lock(&ctx->cmd_lock);
	ret = amsa05rb06_on(ctx);
	if (ret < 0) {
		mutex_unlock(&ctx->cmd_lock);
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(amsa05rb06_supplies),
				       ctx->supplies);
		return ret;
	}
	ctx->prepared = true;
	mutex_unlock(&ctx->cmd_lock);

	dev_info(dev, "panel initialised%s\n",
		 mtp_gamma && ctx->gamma_calibrated ? ", calibrated gamma" : "");

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
		if (mtp_gamma && !ctx->gamma_calibrated)
			schedule_delayed_work(&ctx->calib_work,
					      msecs_to_jiffies(AMSA05RB06_CALIB_DELAY_MS));
		amsa05rb06_status_start(ctx);
	}

	return dsi_ctx.accum_err;
}

static int amsa05rb06_disable(struct drm_panel *panel)
{
	struct amsa05rb06 *ctx = to_amsa05rb06(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };

	cancel_delayed_work_sync(&ctx->calib_work);
	cancel_delayed_work_sync(&ctx->status_work);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	return dsi_ctx.accum_err;
}

static int amsa05rb06_unprepare(struct drm_panel *panel)
{
	struct amsa05rb06 *ctx = to_amsa05rb06(panel);

	mutex_lock(&ctx->cmd_lock);
	ctx->prepared = false;
	mutex_unlock(&ctx->cmd_lock);
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

	guard(mutex)(&ctx->cmd_lock);

	/* Applied by amsa05rb06_on() when the panel is next powered */
	if (!ctx->prepared || ctx->handed_over)
		return 0;

	amsa05rb06_set_brightness(ctx, &dsi_ctx, backlight_get_brightness(bl));

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
		.brightness = (180 - AMSA05RB06_MIN_NITS) * AMSA05RB06_BL_SCALE,
		.max_brightness = AMSA05RB06_BL_MAX,
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
	INIT_DELAYED_WORK(&ctx->calib_work, amsa05rb06_calib_work);
	ret = devm_mutex_init(dev, &ctx->cmd_lock);
	if (ret)
		return ret;
	memcpy(ctx->gamma, ana38401_gamma_nominal, ANA38401_GAMMA_LEN);

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

	cancel_delayed_work_sync(&ctx->calib_work);
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
