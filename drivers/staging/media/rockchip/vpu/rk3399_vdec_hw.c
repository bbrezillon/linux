// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 */

#include <linux/clk.h>

#include "rockchip_vpu.h"
#include "rk3399_vdec_regs.h"

#define RK3399_ACLK_MAX_FREQ (500 * 1000 * 1000)

/*
 * Supported formats.
 */

static const struct rockchip_vpu_fmt rk3399_vdec_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = RK_VPU_MODE_NONE,
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE_ANNEXB,
		.codec_mode = RK_VPU_MODE_H264_DEC,
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
};

static irqreturn_t rk3399_vdec_irq(int irq, void *dev_id)
{
	struct rockchip_vpu_dev *vpu = dev_id;
	u32 status = vdpu_read(vpu, RKVDEC_REG_INTERRUPT);

	vdpu_write(vpu, 0, RKVDEC_REG_INTERRUPT);

	rockchip_vpu_irq_done(vpu,
		0,
		status & RKVDEC_RDY_STA ?
		VB2_BUF_STATE_DONE :
		VB2_BUF_STATE_ERROR);
	return IRQ_HANDLED;
}

static int rk3399_vdec_hw_init(struct rockchip_vpu_dev *vpu)
{
	/* Bump ACLK to max. possible freq. to improve performance. */
	clk_set_rate(vpu->clocks[0].clk, RK3399_ACLK_MAX_FREQ);
	return 0;
}

static void rk3399_vdec_reset(struct rockchip_vpu_ctx *ctx)
{
	struct rockchip_vpu_dev *vpu = ctx->dev;

	vdpu_write(vpu, RKVDEC_IRQ_DIS, RKVDEC_REG_INTERRUPT);
	vdpu_write(vpu, 0, RKVDEC_REG_SYSCTRL);
}

/*
 * Supported codec ops.
 */

static const struct rockchip_vpu_codec_ops rk3399_vdec_codec_ops[] = {
	[RK_VPU_MODE_H264_DEC] = {
		.init = rk3399_vdec_h264_init,
		.exit = rk3399_vdec_h264_exit,
		.run = rk3399_vdec_h264_run,
		.reset = rk3399_vdec_reset,
	},
};

const struct rockchip_vpu_variant rk3399_vdec_variant = {
	.dec_offset = 0x0,
	.dec_fmts = rk3399_vdec_fmts,
	.num_dec_fmts = ARRAY_SIZE(rk3399_vdec_fmts),
	.codec = RK_VPU_H264_DECODER,
	.codec_ops = rk3399_vdec_codec_ops,
	.vdpu_irq = rk3399_vdec_irq,
	.init = rk3399_vdec_hw_init,
	.clk_names = {"aclk", "hclk", "sclk_cabac", "sclk_core"},
	.num_clocks = 4
};
