// SPDX-License-Identifier: GPL-2.0-only
/*
 * Renesas TFT FHD panel (JDI ACX454AKM) as found in the Samsung Galaxy S4
 * Active (GT-I9295, jactivelte).
 *
 * The power-up order, reset pulse, DSI timing and the command sequence were
 * recovered from the stock Samsung aboot (LK) image, which is the only piece
 * of software known to bring this panel up from cold. The downstream kernel
 * for this board relies on the bootloader and never resets the panel itself.
 *
 * aboot: L15 1.8 V, 20 ms, LVS1, 10 ms, GPIO 33, 10 ms, GPIO 20, 10 ms,
 * L16 3.0 V, 10 ms, LED driver enable, 200 ms, then MPP2 high 10 ms, low
 * 10 ms, high 10 ms, then 0x51 0x80, 0x55 0x00, 0x53 0x2c, 0x11, 0x29 with
 * a 906 MHz bit clock, 4 lanes, burst video mode, RGB888,
 * porches 100/100/14 and 6/8/2.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

/* Brightness aboot programs while bringing the panel up (downstream: 0xb6). */
#define RENESAS_TFT_DEFAULT_BRIGHTNESS	0x80

/* Write control display: BCTRL | DD | BL, matching downstream 0x53 0x2c. */
#define RENESAS_TFT_WRCTRLD		0x2c

static bool lpm;
module_param(lpm, bool, 0644);
MODULE_PARM_DESC(lpm, "Send DCS commands in low power mode instead of HS");

static bool ddic_read;
module_param(ddic_read, bool, 0644);
MODULE_PARM_DESC(ddic_read, "Read back panel status registers and log them");

static bool skip_reset;
module_param(skip_reset, bool, 0644);
MODULE_PARM_DESC(skip_reset, "Do not pulse the panel reset line");

static bool skip_power;
module_param(skip_power, bool, 0644);
MODULE_PARM_DESC(skip_power, "Do not touch panel rails and enable pins");

static unsigned int brightness = RENESAS_TFT_DEFAULT_BRIGHTNESS;
module_param(brightness, uint, 0644);
MODULE_PARM_DESC(brightness, "Initial DCS brightness (0-255)");

static bool enable_cmd = true;
module_param(enable_cmd, bool, 0644);
MODULE_PARM_DESC(enable_cmd, "Send DCS display on/off while the video stream runs");

static bool bl_dcs = true;
module_param(bl_dcs, bool, 0644);
MODULE_PARM_DESC(bl_dcs, "Send DCS brightness from the backlight class");

/* aboot never appends EOT packets, but the panel accepts them; keep the knob. */
static bool eot = true;
module_param(eot, bool, 0444);
MODULE_PARM_DESC(eot, "Append DSI EOT packets (default on)");

/*
 * The DSI host runs before prepare() (prepare_prev_first), so the panel gets
 * its rails while the link is already up. With a continuous HS clock the
 * controller powers up seeing HS on the clock lane instead of LP-11 and its
 * video receiver never locks: DCS reads and writes still work and sleep-out
 * takes effect, but the screen stays black after every cold start. With a
 * non-continuous clock the lane idles in LP-11 and the controller comes up
 * every time. aboot avoids the problem by powering the panel with the link
 * down and only then initialising the PHY.
 */
static bool noncont_clk = true;
module_param(noncont_clk, bool, 0444);
MODULE_PARM_DESC(noncont_clk, "Use a non-continuous DSI clock lane (default on)");

enum {
	SUPPLY_IOVDD,	/* pm8921 lvs1 */
	SUPPLY_VDD,	/* pm8921 l15  */
	SUPPLY_AVDD,	/* pm8921 l16  */
	SUPPLY_COUNT,
};

struct renesas_tft {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[SUPPLY_COUNT];
	struct gpio_desc *enable_gpio;
	struct gpio_desc *enable2_gpio;
	struct gpio_desc *backlight_gpio;
	struct gpio_desc *reset_gpio;
	struct backlight_device *backlight;
};

static inline struct renesas_tft *to_renesas_tft(struct drm_panel *panel)
{
	return container_of(panel, struct renesas_tft, panel);
}

static void renesas_tft_dump_regs(struct renesas_tft *ctx, const char *when)
{
	static const struct {
		u8 cmd;
		u8 len;
	} regs[] = {
		{ MIPI_DCS_GET_DISPLAY_ID, 3 },
		{ MIPI_DCS_GET_POWER_MODE, 1 },
		{ MIPI_DCS_GET_ADDRESS_MODE, 1 },
		{ MIPI_DCS_GET_PIXEL_FORMAT, 1 },
		{ MIPI_DCS_GET_DISPLAY_MODE, 1 },
		{ MIPI_DCS_GET_SIGNAL_MODE, 1 },
		{ MIPI_DCS_GET_DIAGNOSTIC_RESULT, 1 },
		{ 0xda, 1 },
		{ 0xdb, 1 },
		{ 0xdc, 1 },
		{ 0xbf, 5 },
	};
	struct device *dev = &ctx->dsi->dev;
	u8 buf[8];
	int i, ret;

	if (!ddic_read)
		return;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		memset(buf, 0, sizeof(buf));
		ret = mipi_dsi_dcs_read(ctx->dsi, regs[i].cmd, buf, regs[i].len);
		if (ret < 0)
			dev_info(dev, "%s: read 0x%02x failed: %d\n", when,
				 regs[i].cmd, ret);
		else
			dev_info(dev, "%s: 0x%02x = %*ph\n", when, regs[i].cmd,
				 ret, buf);
	}
}

static void renesas_tft_reset(struct renesas_tft *ctx)
{
	if (!ctx->reset_gpio || skip_reset)
		return;

	/*
	 * aboot: high 10 ms, low 10 ms, high, then 10 ms before the link.
	 * The first DCS read 10 ms after the last edge came back empty, so
	 * give the controller twice that.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
}

static int renesas_tft_power_on(struct renesas_tft *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_enable(ctx->supplies[SUPPLY_VDD].consumer);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vdd\n");
	msleep(20);

	ret = regulator_enable(ctx->supplies[SUPPLY_IOVDD].consumer);
	if (ret) {
		dev_err(dev, "failed to enable iovdd: %d\n", ret);
		goto err_vdd;
	}
	msleep(10);

	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	msleep(10);

	if (ctx->enable2_gpio) {
		gpiod_set_value_cansleep(ctx->enable2_gpio, 1);
		msleep(10);
	}

	ret = regulator_enable(ctx->supplies[SUPPLY_AVDD].consumer);
	if (ret) {
		dev_err(dev, "failed to enable avdd: %d\n", ret);
		goto err_iovdd;
	}
	msleep(10);

	if (ctx->backlight_gpio)
		gpiod_set_value_cansleep(ctx->backlight_gpio, 1);

	msleep(200);

	return 0;

err_iovdd:
	if (ctx->enable2_gpio)
		gpiod_set_value_cansleep(ctx->enable2_gpio, 0);
	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	regulator_disable(ctx->supplies[SUPPLY_IOVDD].consumer);
err_vdd:
	regulator_disable(ctx->supplies[SUPPLY_VDD].consumer);
	return ret;
}

static void renesas_tft_power_off(struct renesas_tft *ctx)
{
	if (ctx->reset_gpio && !skip_reset)
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);

	if (ctx->backlight_gpio)
		gpiod_set_value_cansleep(ctx->backlight_gpio, 0);

	if (ctx->enable2_gpio) {
		gpiod_set_value_cansleep(ctx->enable2_gpio, 0);
		msleep(10);
	}

	gpiod_set_value_cansleep(ctx->enable_gpio, 0);

	/* Downstream keeps at least 1 ms between VDD off and AVDD off. */
	usleep_range(2000, 3000);

	regulator_disable(ctx->supplies[SUPPLY_AVDD].consumer);
	msleep(10);
	regulator_disable(ctx->supplies[SUPPLY_VDD].consumer);
	regulator_disable(ctx->supplies[SUPPLY_IOVDD].consumer);
}

static int renesas_tft_init_cmds(struct renesas_tft *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct device *dev = &ctx->dsi->dev;

	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, brightness & 0xff);
	/* CABC off, as aboot and the downstream driver do for this variant. */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     RENESAS_TFT_WRCTRLD);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);

	if (dsi_ctx.accum_err) {
		dev_err(dev, "init sequence failed: %d\n", dsi_ctx.accum_err);
		return dsi_ctx.accum_err;
	}

	msleep(120);

	renesas_tft_dump_regs(ctx, "after sleep out");

	return 0;
}

static void renesas_tft_apply_lpm(struct renesas_tft *ctx)
{
	if (lpm)
		ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	else
		ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
}

static int renesas_tft_prepare(struct drm_panel *panel)
{
	struct renesas_tft *ctx = to_renesas_tft(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	dev_info(dev, "prepare: lpm=%d skip_power=%d skip_reset=%d brightness=0x%02x\n",
		 lpm, skip_power, skip_reset, brightness);

	renesas_tft_apply_lpm(ctx);

	if (!skip_power) {
		ret = renesas_tft_power_on(ctx);
		if (ret)
			return ret;
	}

	renesas_tft_reset(ctx);

	renesas_tft_dump_regs(ctx, "after reset");

	ret = renesas_tft_init_cmds(ctx);
	if (ret) {
		if (!skip_power)
			renesas_tft_power_off(ctx);
		return ret;
	}

	return 0;
}

/*
 * The DSI host is already off when unprepare() runs (prepare_prev_first), and
 * a command sent then only times out waiting for the video engine, so the
 * sleep-in goes out from disable() right after display-off, as downstream
 * does with its panel_off_cmds.
 */
static int renesas_tft_unprepare(struct drm_panel *panel)
{
	struct renesas_tft *ctx = to_renesas_tft(panel);

	if (!skip_power)
		renesas_tft_power_off(ctx);

	return 0;
}

static int renesas_tft_enable(struct drm_panel *panel)
{
	struct renesas_tft *ctx = to_renesas_tft(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	if (!enable_cmd)
		return 0;

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int renesas_tft_disable(struct drm_panel *panel)
{
	struct renesas_tft *ctx = to_renesas_tft(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	if (!enable_cmd)
		return 0;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	msleep(40);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	msleep(120);

	return dsi_ctx.accum_err;
}

/*
 * Timing as programmed by aboot: hbp 100, hfp 100, hpw 14, vbp 6, vfp 8,
 * vpw 2, a 1294x1936 total and a 906 MHz bit clock.
 *
 * The DSI PLL feedback divider on this PHY only produces VCO rates that are
 * multiples of 2 MHz, and the pixel RCG accepts a rate only if it matches the
 * PLL output (VCO/2) divided by 1, 2, 3 or 16/3 within 100 kHz. With 24 bpp
 * on 4 lanes the pixel clock is always VCO/6, so the VCO must be a multiple
 * of 6 MHz: 906 MHz gives 151 MHz exactly and 60.3 Hz. The 149.666 MHz that
 * was used before asked for a 897.996 MHz VCO, which the divider truncated
 * to 896 MHz, so every clk_set_rate() on the pixel clock failed with -EINVAL
 * and the RCG kept whatever divider the bootloader had left behind.
 */
static const struct drm_display_mode renesas_tft_mode = {
	.clock = 151000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 100,
	.hsync_end = 1080 + 100 + 14,
	.htotal = 1080 + 100 + 14 + 100,
	.vdisplay = 1920,
	.vsync_start = 1920 + 8,
	.vsync_end = 1920 + 8 + 2,
	.vtotal = 1920 + 8 + 2 + 6,
	.width_mm = 62,
	.height_mm = 111,
};

static int renesas_tft_get_modes(struct drm_panel *panel,
				 struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector,
						    &renesas_tft_mode);
}

static const struct drm_panel_funcs renesas_tft_panel_funcs = {
	.prepare = renesas_tft_prepare,
	.unprepare = renesas_tft_unprepare,
	.enable = renesas_tft_enable,
	.disable = renesas_tft_disable,
	.get_modes = renesas_tft_get_modes,
};

/* The DDIC takes an 8-bit brightness; the 16-bit DCS helper is not used. */
static int renesas_tft_bl_update_status(struct backlight_device *bl)
{
	struct renesas_tft *ctx = bl_get_data(bl);
	u8 level = backlight_get_brightness(bl);
	int ret;

	if (!bl_dcs)
		return 0;

	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 &level, 1);
	return ret < 0 ? ret : 0;
}

static const struct backlight_ops renesas_tft_bl_ops = {
	.update_status = renesas_tft_bl_update_status,
};

static int renesas_tft_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties bl_props = {
		.type = BACKLIGHT_RAW,
		.brightness = RENESAS_TFT_DEFAULT_BRIGHTNESS,
		.max_brightness = 255,
	};
	struct renesas_tft *ctx;
	int ret;

	dev_info(dev, "probe start\n");

	ctx = devm_drm_panel_alloc(dev, struct renesas_tft, panel,
				   &renesas_tft_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supplies[SUPPLY_IOVDD].supply = "iovdd";
	ctx->supplies[SUPPLY_VDD].supply = "vdd";
	ctx->supplies[SUPPLY_AVDD].supply = "avdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get supplies\n");

	/*
	 * Downstream asks the RPM for 100 mA on both LDOs. Without a load
	 * request the RPM keeps a PM8921 LDO in low power mode, which cannot
	 * feed a running panel and lets the DDIC brown out.
	 */
	ret = regulator_set_load(ctx->supplies[SUPPLY_VDD].consumer, 100000);
	if (ret)
		dev_warn(dev, "failed to set vdd load: %d\n", ret);
	ret = regulator_set_load(ctx->supplies[SUPPLY_AVDD].consumer, 100000);
	if (ret)
		dev_warn(dev, "failed to set avdd load: %d\n", ret);

	/*
	 * Leave every line as the bootloader configured it until prepare()
	 * runs, so a panel that is already lit keeps its state at probe.
	 */
	ctx->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "failed to get enable GPIO\n");

	ctx->enable2_gpio = devm_gpiod_get_optional(dev, "enable2", GPIOD_ASIS);
	if (IS_ERR(ctx->enable2_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable2_gpio),
				     "failed to get enable2 GPIO\n");

	ctx->backlight_gpio = devm_gpiod_get_optional(dev, "backlight",
						      GPIOD_ASIS);
	if (IS_ERR(ctx->backlight_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight_gpio),
				     "failed to get backlight GPIO\n");

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	dev_info(dev, "gpios: enable=%d enable2=%d backlight=%d reset=%d\n",
		 gpiod_get_value_cansleep(ctx->enable_gpio),
		 ctx->enable2_gpio ? gpiod_get_value_cansleep(ctx->enable2_gpio) : -1,
		 ctx->backlight_gpio ? gpiod_get_value_cansleep(ctx->backlight_gpio) : -1,
		 ctx->reset_gpio ? gpiod_get_value_cansleep(ctx->reset_gpio) : -1);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	if (!eot)
		dsi->mode_flags |= MIPI_DSI_MODE_NO_EOT_PACKET;
	if (noncont_clk)
		dsi->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;
	dsi->hs_rate = 906000000;
	renesas_tft_apply_lpm(ctx);

	ctx->panel.prepare_prev_first = true;

	ctx->backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
							ctx,
							&renesas_tft_bl_ops,
							&bl_props);
	if (IS_ERR(ctx->backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight),
				     "failed to register backlight\n");

	ctx->panel.backlight = ctx->backlight;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret,
				     "failed to attach to DSI host\n");
	}

	dev_info(dev, "probe done\n");

	return 0;
}

static void renesas_tft_remove(struct mipi_dsi_device *dsi)
{
	struct renesas_tft *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id renesas_tft_of_match[] = {
	{ .compatible = "samsung,renesas-tft-fhd" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, renesas_tft_of_match);

static struct mipi_dsi_driver renesas_tft_driver = {
	.probe = renesas_tft_probe,
	.remove = renesas_tft_remove,
	.driver = {
		.name = "panel-samsung-renesas-tft",
		.of_match_table = renesas_tft_of_match,
	},
};
module_mipi_dsi_driver(renesas_tft_driver);

MODULE_DESCRIPTION("DRM driver for the Samsung jactivelte Renesas TFT FHD panel");
MODULE_LICENSE("GPL");
