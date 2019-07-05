/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Memory-to-memory H264 codec framework for Video for Linux 2.
 *
 * Helper functions for H264 codec devices that use memory buffers for both
 * source and destination.
 *
 * Copyright (c) 2019 Collabora Ltd.
 *
 * Author:
 *	Boris Brezillon <boris.brezillon@collabora.com>
 */

#ifndef _MEDIA_V4L2_MEM2MEM_H264_CODEC_H
#define _MEDIA_V4L2_MEM2MEM_H264_CODEC_H

#include <media/h264-ctrls.h>
#include <media/v4l2-mem2mem-codec.h>

struct v4l2_m2m_h264_decode_run {
	struct v4l2_m2m_codec_run base;
	const struct v4l2_ctrl_h264_decode_params *decode_params;
	const struct v4l2_ctrl_h264_slice_params *slices_params;
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling_matrix;
};

void v4l2_m2m_h264_decode_run_preamble(struct v4l2_m2m_codec_ctx *ctx,
				       struct v4l2_m2m_h264_decode_run *run);

static inline void
v4l2_m2m_h264_decode_run_postamble(struct v4l2_m2m_codec_ctx *ctx,
				   struct v4l2_m2m_h264_decode_run *run)
{
	v4l2_m2m_codec_run_postamble(ctx, &run->base);
}

#define V4L2_M2M_H264_DEC_DECODE_PARAMS_CTRL				\
	{								\
		.per_request = true,					\
		.mandatory = true,					\
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAMS,	\
	}

#define V4L2_M2M_H264_DEC_SLICE_PARAMS_CTRL				\
	{								\
		.per_request = true,					\
		.mandatory = true,					\
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAMS,	\
		.cfg.dims[0] = V4L2_H264_MAX_SLICES_PER_FRAME,		\
	}

#define V4L2_M2M_H264_DEC_SPS_CTRL					\
	{								\
		.per_request = true,					\
		.mandatory = true,					\
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_SPS,			\
	}

#define V4L2_M2M_H264_DEC_PPS_CTRL					\
	{								\
		.per_request = true,					\
		.mandatory = true,					\
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_PPS,			\
	}

#define V4L2_M2M_H264_DEC_SCALING_MATRIX_CTRL				\
	{								\
		.per_request = true,					\
		.mandatory = true,					\
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX,	\
	}

#define V4L2_M2M_H264_DEC_MODE_CTRL(_unsupported_modes, _default_mode)	\
	{								\
		.mandatory = true,					\
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_DECODING_MODE,	\
		.cfg.max = V4L2_MPEG_VIDEO_H264_FRAME_BASED_DECODING,	\
		.cfg.menu_skip_mask = _unsupported_modes,		\
		.cfg.def = _default_mode,				\
	}

#endif /* _MEDIA_V4L2_MEM2MEM_H264_CODEC_H */
