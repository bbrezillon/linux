/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Memory-to-memory codec framework for Video for Linux 2.
 *
 * Helper functions for codec devices that use memory buffers for both source
 * and destination.
 *
 * Copyright (c) 2019 Collabora Ltd.
 *
 * Author:
 *	Boris Brezillon <boris.brezillon@collabora.com>
 */

#ifndef _MEDIA_V4L2_MEM2MEM_CODEC_H
#define _MEDIA_V4L2_MEM2MEM_CODEC_H

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>

struct v4l2_m2m_codec_ctrls {
	const struct v4l2_ctrl_config *mandatory;
	unsigned int nmandatory;
	const struct v4l2_ctrl_config *optional;
	unsigned int noptional;
};

struct v4l2_m2m_codec_coded_fmt {
	u32 fourcc;
	struct v4l2_frmsize_stepwise frmsize;
	const struct v4l2_m2m_codec_ctrls *ctrls;
};

struct v4l2_m2m_codec_ctx {
	struct v4l2_fh fh;
	struct v4l2_m2m_codec_coded_fmt *coded_fmt;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct {
		const struct v4l2_m2m_codec_ctrls *def;
	} ctrls;
	struct v4l2_m2m_dev *m2m_dev;
};

struct v4l2_m2m_codec_run {
	struct {
		struct vb2_v4l2_buffer *src;
        	struct vb2_v4l2_buffer *dst;
	} bufs;
};

int v4l2_m2m_codec_init_ctrls(struct v4l2_m2m_codec_ctx *ctx,
			      struct v4l2_m2m_codec_coded_fmt *fmts,
			      unsigned int nfmts,
			      const struct v4l2_ctrl_config *extra_ctrls,
			      unsigned int nextra_ctrls);
void v4l2_m2m_codec_cleanup_ctrls(struct v4l2_m2m_codec_ctx *ctx);
void v4l2_m2m_codec_open(struct file *file,
			 struct v4l2_m2m_dev *m2m_dev,
			 struct v4l2_m2m_codec_ctx *ctx);
void v4l2_m2m_codec_release(struct v4l2_m2m_codec_ctx *ctx);
void v4l2_m2m_codec_run_preamble(struct v4l2_m2m_codec_ctx *ctx,
                                 struct v4l2_m2m_codec_run *run);
void v4l2_m2m_codec_run_postamble(struct v4l2_m2m_codec_ctx *ctx,
                                  struct v4l2_m2m_codec_run *run);
void v4l2_m2m_codec_job_finish(struct v4l2_m2m_codec_ctx *ctx,
                               enum vb2_buffer_state state);

#endif /* _MEDIA_V4L2_MEM2MEM_CODEC_H */
