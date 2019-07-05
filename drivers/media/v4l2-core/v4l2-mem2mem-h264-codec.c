// SPDX-License-Identifier: GPL-2.0+
/*
 * Memory-to-memory H264 codec framework for Video for Linux 2.
 *
 * Helper functions for H264 codec devices that use memory buffers for both
 * source and destination.
 *
 * Copyright (c) 2019 Collabora Ltd.
 *
 * Author:
 *      Boris Brezillon <boris.brezillon@collabora.com>
 */

#include <linux/types.h>
#include <media/v4l2-mem2mem-h264-codec.h>

void v4l2_m2m_h264_decode_run_preamble(struct v4l2_m2m_codec_ctx *ctx,
				       struct v4l2_m2m_h264_decode_run *run)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAMS);
	run->decode_params = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAMS);
	run->slices_params = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_H264_SPS);
	run->sps = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_H264_PPS);
	run->pps = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX);
	run->scaling_matrix = ctrl ? ctrl->p_cur.p : NULL;

	v4l2_m2m_codec_run_preamble(ctx, &run->base);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_h264_decode_run_preamble);
