/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Memory-to-memory MPEG2 codec framework for Video for Linux 2.
 *
 * Helper functions for MPEG2 codec devices that use memory buffers for both
 * source and destination.
 *
 * Copyright (c) 2019 Collabora Ltd.
 *
 * Author:
 *	Boris Brezillon <boris.brezillon@collabora.com>
 */

#ifndef _MEDIA_V4L2_MEM2MEM_MPEG2_CODEC_H
#define _MEDIA_V4L2_MEM2MEM_MPEG2_CODEC_H

#include <media/mpeg2-ctrls.h>
#include <media/v4l2-mem2mem-codec.h>

struct v4l2_m2m_mpeg2_codec_run {
	struct v4l2_m2m_codec_run base;
	const struct v4l2_ctrl_mpeg2_slice_params *slice_params;
	const struct v4l2_ctrl_mpeg2_quantization *quantization;
};

void v4l2_m2m_mpeg2_codec_run_preamble(struct v4l2_m2m_codec_ctx *ctx,
				       struct v4l2_m2m_mpeg2_codec_run *run);

static inline void
v4l2_m2m_mpeg2_codec_run_postamble(struct v4l2_m2m_codec_ctx *ctx,
				   struct v4l2_m2m_mpeg2_codec_run *run)
{
	v4l2_m2m_codec_run_postamble(ctx, &run->base);
}

#define V4L2_M2M_MPEG2_SLICE_PARAMS_CTRL					\
	{									\
		.id = V4L2_CID_MPEG_VIDEO_MPEG2_SLICE_PARAMS,			\
		.elem_size = sizeof(struct v4l2_ctrl_mpeg2_slice_params),	\
	},

#define V4L2_M2M_MPEG2_QUANTIZATION_CTRL					\
	{									\
		.id = V4L2_CID_MPEG_VIDEO_MPEG2_QUANTIZATION,			\
		.elem_size = sizeof(struct v4l2_ctrl_mpeg2_quantization),	\
	}

const struct v4l2_m2m_codec_coded_fmt_ctrls v4l2_m2m_mpeg2_stateless_codec_std_ctrls;

#endif /* _MEDIA_V4L2_MEM2MEM_CODEC_H */
