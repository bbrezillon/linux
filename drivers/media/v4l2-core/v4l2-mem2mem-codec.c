// SPDX-License-Identifier: GPL-2.0+
/*
 * Memory-to-memory codec framework for Video for Linux 2.
 *
 * Helper functions for codec devices that use memory buffers for both source
 * and destination.
 *
 * Copyright (c) 2019 Collabora Ltd.
 *
 * Author:
 *      Boris Brezillon <boris.brezillon@collabora.com>
 */

#include <media/v4l2-mem2mem-codec.h>

void v4l2_m2m_codec_run_preamble(struct v4l2_m2m_codec_ctx *ctx,
				 struct v4l2_m2m_codec_run *run)
{
	struct media_request *src_req;

	memset(run, 0, sizeof(*run));

	run->bufs.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run->bufs.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* Apply request(s) controls if needed. */
	src_req = run->bufs.src->vb2_buf.req_obj.req;
	if (src_req)
		v4l2_ctrl_request_setup(src_req, &ctx->ctrl_hdl);

	v4l2_m2m_buf_copy_metadata(run->bufs.src, run->bufs.dst, true);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_run_preamble);

void v4l2_m2m_codec_run_postamble(struct v4l2_m2m_codec_ctx *ctx,
				  struct v4l2_m2m_codec_run *run)
{
	struct media_request *src_req = run->bufs.src->vb2_buf.req_obj.req;

	if (src_req)
                v4l2_ctrl_request_complete(src_req, &ctx->ctrl_hdl);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_run_postamble);

void v4l2_m2m_codec_job_finish(struct v4l2_m2m_dev *m2m_dev,
			       struct v4l2_m2m_codec_ctx *ctx,
			       enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src_buf, *dst_buf;

	src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	WARN_ON(!src_buf);
	if (src_buf)
		v4l2_m2m_buf_done(src_buf, state);

	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	WARN_ON(!dst_buf);
	if (dst_buf)
		v4l2_m2m_buf_done(dst_buf, state);

        v4l2_m2m_job_finish(m2m_dev, ctx->fh.m2m_ctx);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_job_finish);
