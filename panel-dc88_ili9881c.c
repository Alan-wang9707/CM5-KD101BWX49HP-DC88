// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017-2018, Bootlin
 * Copyright (C) 2021, Henson Li <henson@cutiepi.io>
 * Copyright (C) 2021, Penk Chen <penk@cutiepi.io>
 * Copyright (C) 2022, Mark Williams <mark@crystalfontz.com>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>


#define KD101_SELF_TEST_MODE 0 /* MOD:SELF_TEST_MODE */


enum ili9881c_op {
	ILI9881C_SWITCH_PAGE,
	ILI9881C_COMMAND,
};


struct ili9881c_instr {
	enum ili9881c_op	op;

	union arg {
		struct cmd {
			u8	cmd;
			u8	data;
		} cmd;
		u8	page;
	} arg;
};

/* 面板行为标志位 */
enum ili9881_desc_flags {

	ILI9881_FLAGS_NO_SHUTDOWN_CMDS = BIT(0),

	ILI9881_FLAGS_PANEL_ON_IN_PREPARE = BIT(1),

	ILI9881_FLAGS_MAX = BIT(31),
};

/* 单个面板型号的静态描述信息 */
struct ili9881c_desc {
	const struct ili9881c_instr *init;
	const size_t init_length;
	const struct drm_display_mode *mode;
	const unsigned long mode_flags;
	unsigned int lanes;
	enum ili9881_desc_flags flags;
};

/* 运行时上下文 */
struct ili9881c {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;
	const struct ili9881c_desc	*desc;

	struct regulator	*power;
	struct gpio_desc	*reset;

	enum drm_panel_orientation	orientation;
};

/* 初始化表宏：切换寄存器页 */
#define ILI9881C_SWITCH_PAGE_INSTR(_page)	\
	{					\
		.op = ILI9881C_SWITCH_PAGE,	\
		.arg = {			\
			.page = (_page),	\
		},				\
	}

/* 初始化表宏：写一条 cmd/data 命令 */
#define ILI9881C_COMMAND_INSTR(_cmd, _data)		\
	{						\
		.op = ILI9881C_COMMAND,		\
		.arg = {				\
			.cmd = {			\
				.cmd = (_cmd),		\
				.data = (_data),	\
			},				\
		},					\
	}

/*
 * 面板初始化指令表。
 * 你当前版本保留为空数组，表示不发送厂商私有初始化序列。
 */
static const struct ili9881c_instr k101_im2byl02_init[] = {

};


/* drm_panel 转回私有上下文 */
static inline struct ili9881c *panel_to_ili9881c(struct drm_panel *panel)
{
	return container_of(panel, struct ili9881c, panel);
}

/*
 * The panel seems to accept some private DCS commands that map
 * directly to registers.
 *
 * It is organised by page, with each page having its own set of
 * registers, and the first page looks like it's holding the standard
 * DCS commands.
 *
 * So before any attempt at sending a command or data, we have to be
 * sure if we're in the right page or not.
 */
static int ili9881c_switch_page(struct ili9881c *ctx, u8 page)
{
	u8 buf[4] = { 0xff, 0x98, 0x81, page };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}


static int ili9881c_send_cmd_data(struct ili9881c *ctx, u8 cmd, u8 data)
{
	u8 buf[2] = { cmd, data };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}


static int ili9881c_prepare(struct drm_panel *panel)
{
	struct ili9881c *ctx = panel_to_ili9881c(panel);
	u8 dcs_read;
	int ret;

	dev_info(&ctx->dsi->dev,
		 "[3.1] prepare: begin (init_len=%zu, flags=0x%lx, reset=%s)\n",
		 ctx->desc->init_length, (unsigned long)ctx->desc->flags,
		 ctx->reset ? "present" : "absent");


	ret = regulator_enable(ctx->power);
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E1] prepare: regulator_enable failed: %d\n", ret);
		return ret;
	}
	dev_info(&ctx->dsi->dev, "[3.3] prepare: power enabled, wait 50ms\n");
	msleep(20);


	gpiod_set_value_cansleep(ctx->reset, 1);
	dev_info(&ctx->dsi->dev,
		 "[3.2] prepare: assert reset and wait 200ms\n");
	msleep(10); 


	gpiod_set_value_cansleep(ctx->reset, 0);
	dev_info(&ctx->dsi->dev,
		 "[3.4] prepare: release reset and wait 150ms\n");
	msleep(20);


#if KD101_SELF_TEST_MODE /* MOD:SELF_TEST_MODE */
	dev_info(&ctx->dsi->dev, "[3.4.1] self-test: enter page 4\n");
	ret = ili9881c_switch_page(ctx, 4);
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E2] self-test: switch page 4 failed: %d\n", ret);
		return ret;
	}

	ret = ili9881c_send_cmd_data(ctx, 0x2F, 0x00);
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E3] self-test: write 2F=00 failed: %d\n", ret);
		return ret;
	}

	ret = ili9881c_send_cmd_data(ctx, 0x2D, 0x00);
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E4] self-test: write 2D=00 failed: %d\n", ret);
		return ret;
	}

	msleep(200);

	ret = ili9881c_send_cmd_data(ctx, 0x2D, 0x1F);
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E5] self-test: write 2D=1F failed: %d\n", ret);
		return ret;
	}

	ret = ili9881c_send_cmd_data(ctx, 0x2F, 0x11);
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E6] self-test: write 2F=11 failed: %d\n", ret);
		return ret;
	}

	dev_info(&ctx->dsi->dev, "[3.4.2] self-test: page4 sequence done\n");
#endif /* MOD:SELF_TEST_MODE */


	ret = ili9881c_switch_page(ctx, 0); 
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E7] prepare: switch page 0 failed: %d\n", ret);
		return ret;
	}
	dev_info(&ctx->dsi->dev, "[3.5] prepare: switched to DCS page 0\n");

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi); /* MOD:MANUAL_DCS_11 */
	/* ret = mipi_dsi_dcs_write_buffer(ctx->dsi, "\x11", 1); */ /* MOD:MANUAL_DCS_11 */
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.E8] prepare: sleep out (0x11) failed: %d\n", ret);
		return ret;
	}
	dev_info(&ctx->dsi->dev, "[3.6] prepare: sent 0x11 sleep out, wait 120ms\n");
	msleep(120); /* 必须等够 */

	/* 回读 DCS 状态：确认面板是否真正接收命令 */ /* MOD:DCS_READBACK_VERIFY */
	ret = mipi_dsi_dcs_read(ctx->dsi, 0x0A, &dcs_read, 1); /* Power Mode */ /* MOD:DCS_READBACK_VERIFY */
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "[3.E4] prepare: read 0x0A power mode failed: %d\n", ret); /* MOD:DCS_READBACK_VERIFY */
	else
		dev_info(&ctx->dsi->dev, "[3.6.1] prepare: read 0x0A power mode = 0x%02x\n", dcs_read); /* MOD:DCS_READBACK_VERIFY */

	ret = mipi_dsi_dcs_read(ctx->dsi, 0x0D, &dcs_read, 1); /* Display Mode */ /* MOD:DCS_READBACK_VERIFY */
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "[3.E5] prepare: read 0x0D display mode failed: %d\n", ret); /* MOD:DCS_READBACK_VERIFY */
	else
		dev_info(&ctx->dsi->dev, "[3.6.2] prepare: read 0x0D display mode = 0x%02x\n", dcs_read); /* MOD:DCS_READBACK_VERIFY */

	ret = mipi_dsi_dcs_read(ctx->dsi, 0x0C, &dcs_read, 1); /* Pixel Format */ /* MOD:DCS_READBACK_VERIFY */
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "[3.E6] prepare: read 0x0C pixel format failed: %d\n", ret); /* MOD:DCS_READBACK_VERIFY */
	else
		dev_info(&ctx->dsi->dev, "[3.6.3] prepare: read 0x0C pixel format = 0x%02x\n", dcs_read); /* MOD:DCS_READBACK_VERIFY */


	ret = mipi_dsi_dcs_set_display_on(ctx->dsi); /* MOD:MANUAL_DCS_29 */
	/* ret = mipi_dsi_dcs_write_buffer(ctx->dsi, "\x29", 1); */ /* MOD:MANUAL_DCS_29 */
	if (ret) {
		dev_err(&ctx->dsi->dev, "[3.W1] prepare: display on (0x29) failed: %d\n", ret);

	} else {
		dev_info(&ctx->dsi->dev, "[3.7] prepare: sent 0x29 display on\n");
	}
	msleep(120); 

	dev_info(&ctx->dsi->dev, "[3.8] prepare: done\n");
	return 0;
}


static int ili9881c_enable(struct drm_panel *panel)
{
	struct ili9881c *ctx = panel_to_ili9881c(panel);
	dev_info(&ctx->dsi->dev,
		 "[4.1] enable: skip (already handled in prepare)\n");
	return 0;
}


static int ili9881c_disable(struct drm_panel *panel)
{
	struct ili9881c *ctx = panel_to_ili9881c(panel);

	dev_info(&ctx->dsi->dev, "[5.1] disable: begin\n");

	if (!(ctx->desc->flags & ILI9881_FLAGS_PANEL_ON_IN_PREPARE))
		mipi_dsi_dcs_set_display_off(ctx->dsi);
	dev_info(&ctx->dsi->dev, "[5.2] disable: display off sent (if needed)\n");

	dev_info(&ctx->dsi->dev, "[5.3] disable: done\n");
	return 0;
}


static int ili9881c_unprepare(struct drm_panel *panel)
{
	struct ili9881c *ctx = panel_to_ili9881c(panel);

	dev_info(&ctx->dsi->dev, "[6.1] unprepare: begin\n");

	if (!(ctx->desc->flags & ILI9881_FLAGS_NO_SHUTDOWN_CMDS)) {
		if (ctx->desc->flags & ILI9881_FLAGS_PANEL_ON_IN_PREPARE)
			mipi_dsi_dcs_set_display_off(ctx->dsi);


		mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
		dev_info(&ctx->dsi->dev, "[6.2] unprepare: display off + sleep in sent\n");
	}

	regulator_disable(ctx->power);

	gpiod_set_value_cansleep(ctx->reset, 1);

	dev_info(&ctx->dsi->dev, "[6.3] unprepare: power off and reset asserted\n");
	return 0;
}



/* 固定显示时序（800x1280） */
static const struct drm_display_mode k101_im2byl02_default_mode = {
	.clock		= 72000,

	.hdisplay	= 800,
	.hsync_start	= 800 + 40,
	.hsync_end	= 800 + 40 + 20,
	.htotal		= 800 + 40 + 20 + 20,

	.vdisplay	= 1280,
	.vsync_start	= 1280 + 30,
	.vsync_end	= 1280 + 30 + 4,
	.vtotal		= 1280 + 30 + 4 + 12,

	.width_mm	= 135,
	.height_mm	= 217,
};


static int ili9881c_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct ili9881c *ctx = panel_to_ili9881c(panel);
	struct drm_display_mode *mode;

	dev_info(&ctx->dsi->dev, "[2.1] get_modes: begin\n");

	mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "[2.E1] get_modes: failed to duplicate mode %ux%ux@%u\n",
			ctx->desc->mode->hdisplay,
			ctx->desc->mode->vdisplay,
			drm_mode_vrefresh(ctx->desc->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, ctx->orientation);

	dev_info(&ctx->dsi->dev, "[2.2] get_modes: added %ux%u@%u\n",
		 mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode));
	return 1;
}

/* 返回当前面板安装方向 */
static enum drm_panel_orientation ili9881c_get_orientation(struct drm_panel *panel)
{
	struct ili9881c *ctx = panel_to_ili9881c(panel);

	return ctx->orientation;
}

/* panel 回调集合 */
static const struct drm_panel_funcs ili9881c_funcs = {
	.prepare	= ili9881c_prepare,
	.unprepare	= ili9881c_unprepare,
	.enable		= ili9881c_enable,
	.disable	= ili9881c_disable,
	.get_modes	= ili9881c_get_modes,
	.get_orientation = ili9881c_get_orientation,
};

static int ili9881c_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct ili9881c *ctx;
	int ret;

	dev_info(&dsi->dev, "[1.1] probe: begin\n");

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		dev_err(&dsi->dev, "[1.E1] probe: alloc context failed\n");
		return -ENOMEM;
	}
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;
	ctx->desc = of_device_get_match_data(&dsi->dev);
	dev_info(&dsi->dev, "[1.2] probe: match data ready\n");

	ctx->panel.prepare_prev_first = true;
	drm_panel_init(&ctx->panel, &dsi->dev, &ili9881c_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->power = devm_regulator_get(&dsi->dev, "power");
	if (IS_ERR(ctx->power))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->power),
				     "Couldn't get our power regulator\n");
	dev_info(&dsi->dev, "[1.3] probe: power regulator acquired\n");

	/* 可选：reset GPIO（名称 reset） */
	//ctx->reset = devm_gpiod_get_optional(&dsi->dev, "reset", GPIOD_OUT_HIGH);//拉低reset引脚
	ctx->reset = devm_gpiod_get_optional(&dsi->dev, "reset", GPIOD_OUT_LOW);//拉高reset引脚
	if (IS_ERR(ctx->reset))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->reset),
				     "Couldn't get our reset GPIO\n");
	dev_info(&dsi->dev, "[1.4] probe: reset gpio %s\n",
		 ctx->reset ? "acquired" : "not provided");

	/* 读取面板方向（rotation） */
	ret = of_drm_get_panel_orientation(dsi->dev.of_node, &ctx->orientation);
	if (ret) {
		dev_err(&dsi->dev, "[1.E2] %pOF: failed to get orientation: %d\n",
			dsi->dev.of_node, ret);
		return ret;
	}
	dev_info(&dsi->dev, "[1.5] probe: orientation read done\n");

	ctx->panel.prepare_prev_first = true;

	/* 关联设备树 backlight（可选） */
	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;
	dev_info(&dsi->dev, "[1.6] probe: backlight linked\n");


	drm_panel_add(&ctx->panel);

	/* 下发 DSI 链路参数 */
	dsi->mode_flags = ctx->desc->mode_flags;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = ctx->desc->lanes;
	dev_info(&dsi->dev,
		 "[1.7] probe: dsi cfg lanes=%u format=RGB888 mode_flags=0x%lx (base+DT)\n",
		 dsi->lanes, dsi->mode_flags); /* MOD:MODE_FLAGS_VERIFY */

	/* 连接到 DSI Host */
	ret = mipi_dsi_attach(dsi);
	if (ret) {
		dev_err(&dsi->dev, "[1.E3] probe: dsi attach failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
	} else {
		dev_info(&dsi->dev, "[1.8] probe: dsi attach done\n");
	}

	dev_info(&dsi->dev, "[1.9] probe: done\n");
	return ret;
}

/* DSI remove：与 probe 对称释放运行时资源 */
static void ili9881c_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ili9881c *ctx = mipi_dsi_get_drvdata(dsi);

	dev_info(&dsi->dev, "[7.1] remove: begin\n");
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->power);
	dev_info(&dsi->dev, "[7.2] remove: done\n");
}

/* 当前唯一保留面板的描述信息 */
static const struct ili9881c_desc kD101_dc88_desc = {
	.init = k101_im2byl02_init,
	.init_length = ARRAY_SIZE(k101_im2byl02_init),
	.mode = &k101_im2byl02_default_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		      MIPI_DSI_MODE_LPM,
	//.mode_flags =  MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM,		  
	.lanes = 4,
};

/* OF 兼容表：仅匹配 kdxs,k101-dc88 */
static const struct of_device_id ili9881c_of_match[] = {
	{ .compatible = "kdxs,k101-dc88", .data = &kD101_dc88_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, ili9881c_of_match);

/* MIPI DSI 驱动入口 */
static struct mipi_dsi_driver ili9881c_dsi_driver = {
	.probe		= ili9881c_dsi_probe,
	.remove		= ili9881c_dsi_remove,
	.driver = {
		.name		= "ili9881c-dsi",
		.of_match_table	= ili9881c_of_match,
	},
};
module_mipi_dsi_driver(ili9881c_dsi_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Ilitek ILI9881C Controller Driver");
MODULE_LICENSE("GPL v2");
