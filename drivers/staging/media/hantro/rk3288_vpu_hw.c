// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 */

#include <linux/clk.h>

#include "hantro.h"
#include "hantro_jpeg.h"
#include "hantro_g1_regs.h"
#include "hantro_h1_regs.h"

#define RK3288_ACLK_MAX_FREQ (400 * 1000 * 1000)

/*
 * Supported formats.
 */

#define FRMSIZE(min_w, max_w, step_w, min_h, max_h, step_h)      \
        &((struct v4l2_frmsize_stepwise) {                      \
                .min_width = min_w,                             \
                .max_width = max_w,                             \
                .step_width = step_w,                           \
                .min_height = min_h,                            \
                .max_height = max_h,                            \
                .step_height = step_h,                          \
        })

static const struct v4l2_m2m_codec_decoded_fmt_desc rk3288_enc_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUV420M,
		.priv = HANTRO_FMT(RK3288_VPU_ENC_FMT_YUV420P),
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.priv = HANTRO_FMT(RK3288_VPU_ENC_FMT_YUV420SP),
	},
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.priv = HANTRO_FMT(RK3288_VPU_ENC_FMT_YUYV422),
	},
	{
		.fourcc = V4L2_PIX_FMT_UYVY,
		.priv = HANTRO_FMT(RK3288_VPU_ENC_FMT_UYVY422),
	},
};

static const struct v4l2_m2m_codec_coded_fmt_desc rk3288_enc_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_JPEG,
		.frmsize = FRMSIZE(96, 8192, JPEG_MB_DIM, 8192, JPEG_MB_DIM),
		.ctrls = v4l2_m2m_codec_jpeg_ctrls,
		.adjust_fmt = hantro_h1_jpeg_enc_adjust_fmt,
		.priv = HANTRO_FMT(HANTRO_MODE_JPEG_ENC),
	},
}

static const struct v4l2_m2m_codec_decoded_fmt_desc rk3288_dec_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
	},
};

static const struct v4l2_m2m_codec_coded_fmt_desc rk3288_dec_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE_RAW,
		.codec_mode = HANTRO_MODE_H264_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 48,
			.max_width = 3840,
			.step_width = H264_MB_DIM,
			.min_height = 48,
			.max_height = 2160,
			.step_height = H264_MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_MPEG2_SLICE,
		.adjust_fmt = hantro_g1_mpeg2_dec_adjust_fmt,
		.frmsize = FRMSIZE(48, 1920, MPEG2_MB_DIM, 48, 1088, MPEG2_MB_DIM),
		.priv = HANTRO_FMT(HANTRO_MODE_MPEG2_DEC),
	},
	{
		.fourcc = V4L2_PIX_FMT_VP8_FRAME,
		.adjust_fmt = hantro_g1_vp8_dec_adjust_fmt,
		.frmsize = FRMSIZE(48, 3840, 16, 48, 2160, 16),
		.priv = HANTRO_FMT(HANTRO_MODE_VP8_DEC),
	},
	{
		.fourcc = V4L2_PIX_FMT_VP8_FRAME,
		.codec_mode = HANTRO_MODE_VP8_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 48,
			.max_width = 3840,
			.step_width = 16,
			.min_height = 48,
			.max_height = 2160,
			.step_height = 16,
		},
	},
};

static irqreturn_t rk3288_vepu_irq(int irq, void *dev_id)
{
	struct hantro_dev *vpu = dev_id;
	enum vb2_buffer_state state;
	u32 status, bytesused;

	status = vepu_read(vpu, H1_REG_INTERRUPT);
	bytesused = vepu_read(vpu, H1_REG_STR_BUF_LIMIT) / 8;
	state = (status & H1_REG_INTERRUPT_FRAME_RDY) ?
		VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;

	vepu_write(vpu, 0, H1_REG_INTERRUPT);
	vepu_write(vpu, 0, H1_REG_AXI_CTRL);

	hantro_irq_done(vpu, bytesused, state);

	return IRQ_HANDLED;
}

static irqreturn_t rk3288_vdpu_irq(int irq, void *dev_id)
{
	struct hantro_dev *vpu = dev_id;
	enum vb2_buffer_state state;
	u32 status;

	status = vdpu_read(vpu, G1_REG_INTERRUPT);
	state = (status & G1_REG_INTERRUPT_DEC_RDY_INT) ?
		VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;

	vdpu_write(vpu, 0, G1_REG_INTERRUPT);
	vdpu_write(vpu, G1_REG_CONFIG_DEC_CLK_GATE_E, G1_REG_CONFIG);

	hantro_irq_done(vpu, 0, state);

	return IRQ_HANDLED;
}

static int rk3288_vpu_hw_init(struct hantro_dev *vpu)
{
	/* Bump ACLK to max. possible freq. to improve performance. */
	clk_set_rate(vpu->clocks[0].clk, RK3288_ACLK_MAX_FREQ);
	return 0;
}

static void rk3288_vpu_enc_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	vepu_write(vpu, H1_REG_INTERRUPT_DIS_BIT, H1_REG_INTERRUPT);
	vepu_write(vpu, 0, H1_REG_ENC_CTRL);
	vepu_write(vpu, 0, H1_REG_AXI_CTRL);
}

static void rk3288_vpu_dec_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	vdpu_write(vpu, G1_REG_INTERRUPT_DEC_IRQ_DIS, G1_REG_INTERRUPT);
	vdpu_write(vpu, G1_REG_CONFIG_DEC_CLK_GATE_E, G1_REG_CONFIG);
	vdpu_write(vpu, 1, G1_REG_SOFT_RESET);
}

/*
 * Supported codec ops.
 */

static const struct hantro_codec_ops rk3288_vpu_codec_ops[] = {
	[HANTRO_MODE_JPEG_ENC] = {
		.run = hantro_h1_jpeg_enc_run,
		.reset = rk3288_vpu_enc_reset,
		.init = hantro_jpeg_enc_init,
		.exit = hantro_jpeg_enc_exit,
	},
	[HANTRO_MODE_H264_DEC] = {
		.run = hantro_g1_h264_dec_run,
		.reset = rk3288_vpu_dec_reset,
		.init = hantro_h264_dec_init,
		.exit = hantro_h264_dec_exit,
	},
	[HANTRO_MODE_MPEG2_DEC] = {
		.run = hantro_g1_mpeg2_dec_run,
		.reset = rk3288_vpu_dec_reset,
		.init = hantro_mpeg2_dec_init,
		.exit = hantro_mpeg2_dec_exit,
	},
	[HANTRO_MODE_VP8_DEC] = {
		.run = hantro_g1_vp8_dec_run,
		.reset = rk3288_vpu_dec_reset,
		.init = hantro_vp8_dec_init,
		.exit = hantro_vp8_dec_exit,
	},
};

/*
 * VPU variant.
 */

static const struct hantro_irq rk3288_irqs[] = {
	{ "vepu", rk3288_vepu_irq },
	{ "vdpu", rk3288_vdpu_irq },
};

static const char * const rk3288_clk_names[] = {
	"aclk", "hclk"
};

const struct hantro_variant rk3288_vpu_variant = {
	.enc_offset = 0x0,
	.enc_fmts = rk3288_vpu_enc_fmts,
	.num_enc_fmts = ARRAY_SIZE(rk3288_vpu_enc_fmts),
	.dec_offset = 0x400,
	.dec_fmts = rk3288_vpu_dec_fmts,
	.num_dec_fmts = ARRAY_SIZE(rk3288_vpu_dec_fmts),
	.codec = HANTRO_JPEG_ENCODER | HANTRO_MPEG2_DECODER |
		 HANTRO_VP8_DECODER | HANTRO_H264_DECODER,
	.codec_ops = rk3288_vpu_codec_ops,
	.irqs = rk3288_irqs,
	.num_irqs = ARRAY_SIZE(rk3288_irqs),
	.init = rk3288_vpu_hw_init,
	.clk_names = rk3288_clk_names,
	.num_clocks = ARRAY_SIZE(rk3288_clk_names)
};
