// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver
 *
 * Copyright (C) 2019 Pengutronix, Philipp Zabel <kernel@pengutronix.de>
 */

#include <linux/clk.h>
#include <linux/delay.h>

#include "hantro.h"
#include "hantro_jpeg.h"
#include "rk3288_vpu_regs.h"

#define CTRL_SOFT_RESET		0x00
#define RESET_G1		BIT(1)
#define RESET_G2		BIT(0)
#define RESET_H1		BIT(2)

#define CTRL_CLOCK_ENABLE	0x04
#define CLOCK_G1		BIT(1)
#define CLOCK_G2		BIT(0)
#define CLOCK_H1		BIT(2)

#define CTRL_G1_DEC_FUSE	0x08
#define CTRL_G1_PP_FUSE		0x0c
#define CTRL_G2_DEC_FUSE	0x10
#define CTRL_H1_ENC_FUSE	0x14

static void imx8m_soft_reset(struct hantro_dev *vpu, u32 reset_bits)
{
	u32 val;

	/* Assert */
	val = readl(vpu->ctrl_base + CTRL_SOFT_RESET);
	val &= ~reset_bits;
	writel(val, vpu->ctrl_base + CTRL_SOFT_RESET);

	udelay(2);

	/* Release */
	val = readl(vpu->ctrl_base + CTRL_SOFT_RESET);
	val |= reset_bits;
	writel(val, vpu->ctrl_base + CTRL_SOFT_RESET);
}

static void imx8m_clk_enable(struct hantro_dev *vpu, u32 clock_bits)
{
	u32 val;

	val = readl(vpu->ctrl_base + CTRL_CLOCK_ENABLE);
	val |= clock_bits;
	writel(val, vpu->ctrl_base + CTRL_CLOCK_ENABLE);
}

static int imx8mq_runtime_resume(struct hantro_dev *vpu)
{
	int ret;

	ret = clk_bulk_prepare_enable(vpu->variant->num_clocks, vpu->clocks);
	if (ret) {
		dev_err(vpu->dev, "Failed to enable clocks\n");
		return ret;
	}

	imx8m_soft_reset(vpu, RESET_G1 | RESET_G2);
	imx8m_clk_enable(vpu, CLOCK_G1 | CLOCK_G2);

	/* Set values of the fuse registers */
	writel(0xffffffff, vpu->ctrl_base + CTRL_G1_DEC_FUSE);
	writel(0xffffffff, vpu->ctrl_base + CTRL_G1_PP_FUSE);
	writel(0xffffffff, vpu->ctrl_base + CTRL_G2_DEC_FUSE);

	clk_bulk_disable_unprepare(vpu->variant->num_clocks, vpu->clocks);

	return 0;
}

static int imx8mm_runtime_resume(struct hantro_dev *vpu)
{
	int ret;

	ret = clk_bulk_prepare_enable(vpu->variant->num_clocks, vpu->clocks);
	if (ret) {
		dev_err(vpu->dev, "Failed to enable clocks\n");
		return ret;
	}

	imx8m_soft_reset(vpu, RESET_G1 | RESET_G2 | RESET_H1);
	imx8m_clk_enable(vpu, CLOCK_G1 | CLOCK_G2 | RESET_H1);

	/* Set values of the fuse registers */
	writel(0xffffffff, vpu->ctrl_base + CTRL_G1_DEC_FUSE);
	writel(0xffffffff, vpu->ctrl_base + CTRL_G1_PP_FUSE);
	writel(0xffffffff, vpu->ctrl_base + CTRL_G2_DEC_FUSE);
	writel(0xffffffff, vpu->ctrl_base + CTRL_H1_ENC_FUSE);

	clk_bulk_disable_unprepare(vpu->variant->num_clocks, vpu->clocks);

	return 0;
}

/*
 * Supported formats.
 */

static const struct hantro_fmt imx8m_vpu_dec_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = HANTRO_MODE_NONE,
	},
	{
		.fourcc = V4L2_PIX_FMT_MPEG2_SLICE,
		.codec_mode = HANTRO_MODE_MPEG2_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 48,
			.max_width = 1920,
			.step_width = MPEG2_MB_DIM,
			.min_height = 48,
			.max_height = 1088,
			.step_height = MPEG2_MB_DIM,
		},
	},
};

static const struct hantro_fmt imx8mm_vpu_enc_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUV420M,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = RK3288_VPU_ENC_FMT_YUV420P,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = RK3288_VPU_ENC_FMT_YUV420SP,
	},
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = RK3288_VPU_ENC_FMT_YUYV422,
	},
	{
		.fourcc = V4L2_PIX_FMT_UYVY,
		.codec_mode = HANTRO_MODE_NONE,
		.enc_fmt = RK3288_VPU_ENC_FMT_UYVY422,
	},
	{
		.fourcc = V4L2_PIX_FMT_JPEG,
		.codec_mode = HANTRO_MODE_JPEG_ENC,
		.max_depth = 2,
		.header_size = JPEG_HEADER_SIZE,
		.frmsize = {
			.min_width = 96,
			.max_width = 8192,
			.step_width = JPEG_MB_DIM,
			.min_height = 32,
			.max_height = 8192,
			.step_height = JPEG_MB_DIM,
		},
	},
};

static irqreturn_t imx8m_vpu_g1_irq(int irq, void *dev_id)
{
	struct hantro_dev *vpu = dev_id;
	enum vb2_buffer_state state;
	u32 status;

	status = vdpu_read(vpu, VDPU_REG_INTERRUPT);
	state = (status & VDPU_REG_INTERRUPT_DEC_RDY_INT) ?
		 VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;

	vdpu_write(vpu, 0, VDPU_REG_INTERRUPT);
	vdpu_write(vpu, VDPU_REG_CONFIG_DEC_CLK_GATE_E, VDPU_REG_CONFIG);

	hantro_irq_done(vpu, 0, state);

	return IRQ_HANDLED;
}

static irqreturn_t imx8mm_vpu_h1_irq(int irq, void *dev_id)
{
	struct hantro_dev *vpu = dev_id;
	enum vb2_buffer_state state;
	u32 status, bytesused;

	status = vepu_read(vpu, VEPU_REG_INTERRUPT);
	bytesused = vepu_read(vpu, VEPU_REG_STR_BUF_LIMIT) / 8;
	state = (status & VEPU_REG_INTERRUPT_FRAME_RDY) ?
		 VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;

	vepu_write(vpu, 0, VEPU_REG_INTERRUPT);
	vepu_write(vpu, 0, VEPU_REG_AXI_CTRL);

	hantro_irq_done(vpu, bytesused, state);

	return IRQ_HANDLED;
}

static int imx8mq_vpu_hw_init(struct hantro_dev *vpu)
{
	vpu->dec_base = vpu->bases[0];
	vpu->ctrl_base = vpu->bases[vpu->variant->num_regs - 1];

	return 0;
}

static int imx8mm_vpu_hw_init(struct hantro_dev *vpu)
{
	vpu->dec_base = vpu->bases[0];
	vpu->enc_base = vpu->bases[2];
	vpu->ctrl_base = vpu->bases[vpu->variant->num_regs - 1];

	return 0;
}

static void imx8m_vpu_g1_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	imx8m_soft_reset(vpu, RESET_G1);
}

static void imx8mm_vpu_h1_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	imx8m_soft_reset(vpu, RESET_H1);
}

/*
 * Supported codec ops.
 */

static const struct hantro_codec_ops imx8mq_vpu_codec_ops[] = {
	[HANTRO_MODE_MPEG2_DEC] = {
		.run = hantro_g1_mpeg2_dec_run,
		.reset = imx8m_vpu_g1_reset,
		.init = hantro_mpeg2_dec_init,
		.exit = hantro_mpeg2_dec_exit,
	},
};

static const struct hantro_codec_ops imx8mm_vpu_codec_ops[] = {
	[HANTRO_MODE_MPEG2_DEC] = {
		.run = hantro_g1_mpeg2_dec_run,
		.reset = imx8m_vpu_g1_reset,
		.init = hantro_mpeg2_dec_init,
		.exit = hantro_mpeg2_dec_exit,
	},
	[HANTRO_MODE_JPEG_ENC] = {
		.run = hantro_h1_jpeg_enc_run,
		.reset = imx8mm_vpu_h1_reset,
		.init = hantro_jpeg_enc_init,
		.exit = hantro_jpeg_enc_exit,
	},
};

/*
 * VPU variants.
 */

static const struct hantro_irq imx8mq_irqs[] = {
	{ "g1", imx8m_vpu_g1_irq },
	{ "g2", NULL /* TODO: imx8m_vpu_g2_irq */ },
};

static const char * const imx8mq_reg_names[] = { "g1", "g2", "ctrl" };

const struct hantro_variant imx8mq_vpu_variant = {
	.dec_fmts = imx8m_vpu_dec_fmts,
	.num_dec_fmts = ARRAY_SIZE(imx8m_vpu_dec_fmts),
	.codec = HANTRO_MPEG2_DECODER,
	.codec_ops = imx8mq_vpu_codec_ops,
	.init = imx8mq_vpu_hw_init,
	.runtime_resume = imx8mq_runtime_resume,
	.irqs = imx8mq_irqs,
	.num_irqs = ARRAY_SIZE(imx8mq_irqs),
	.num_irqs = 2,
	.clk_names = { "g1", "g2", "bus" },
	.num_clocks = 3,
	.reg_names = imx8mq_reg_names,
	.num_regs = ARRAY_SIZE(imx8mq_reg_names)
};

static const struct hantro_irq imx8mm_irqs[] = {
	{ "g1", imx8m_vpu_g1_irq },
	{ "g2", NULL /* TODO: imx8m_vpu_g2_irq */ },
	{ "h1", imx8mm_vpu_h1_irq },
};

static const char * const imx8mm_reg_names[] = { "g1", "g2", "h1", "ctrl" };

const struct hantro_variant imx8mm_vpu_variant = {
	.dec_fmts = imx8m_vpu_dec_fmts,
	.num_dec_fmts = ARRAY_SIZE(imx8m_vpu_dec_fmts),
	.codec = HANTRO_MPEG2_DECODER,
	.codec_ops = imx8mm_vpu_codec_ops,
	.init = imx8mm_vpu_hw_init,
	.runtime_resume = imx8mm_runtime_resume,
	.irqs = imx8mm_irqs,
	.num_irqs = ARRAY_SIZE(imx8mm_irqs),
	.clk_names = { "g1", "g2", "h1", "bus" },
	.num_clocks = 4,
	.reg_names = imx8mm_reg_names,
	.num_regs = ARRAY_SIZE(imx8mm_reg_names)
};
