// SPDX-License-Identifier: GPL-2.0+
/*
 * Memory-to-memory MPEG2 codec framework for Video for Linux 2.
 *
 * Helper functions for MPEG2 codec devices that use memory buffers for both
 * source and destination.
 *
 * Copyright (c) 2019 Collabora Ltd.
 *
 * Author:
 *      Boris Brezillon <boris.brezillon@collabora.com>
 */

#include <linux/types.h>
#include <media/v4l2-mem2mem-mpeg2-codec.h>

void v4l2_m2m_mpeg2_codec_run_preamble(struct v4l2_m2m_codec_ctx *ctx,
				       struct v4l2_m2m_mpeg2_codec_run *run)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_MPEG2_SLICE_PARAMS);
	run->slice_params = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_MPEG2_QUANTIZATION);
	run->quantization = ctrl ? ctrl->p_cur.p : NULL;

	v4l2_m2m_codec_run_preamble(ctx, &run->base);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_mpeg2_codec_run_preamble);

const struct v4l2_m2m_codec_coded_fmt_ctrls v4l2_m2m_mpeg2_stateless_codec_std_ctrls =
	V4L2_M2M_CODEC_CODED_FMT_CTRLS(
		V4L2_M2M_CODEC_CTRLS(V4L2_M2M_MPEG2_SLICE_PARAMS_CTRL),
		V4L2_M2M_CODEC_CTRLS(V4L2_M2M_MPEG2_QUANTIZATION_CTRL));
EXPORT_SYMBOL_GPL(v4l2_m2m_mpeg2_stateless_codec_std_ctrls);
