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

#include <media/v4l2-common.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-mem2mem-codec.h>

static int v4l2_m2m_codec_add_ctrls(struct v4l2_m2m_codec_ctx *ctx,
				    const struct v4l2_ctrl_config *ctrls,
				    unsigned int nctrls)
{
	unsigned int i;

	if (nctrls && !ctrls)
		return -EINVAL;

	for (i = 0; i < nctrls; i++) {
		const struct v4l2_ctrl_config *cfg = &ctrls[i];

		if (cfg->type == V4L2_CTRL_TYPE_MENU ||
		    cfg->type == V4L2_CTRL_TYPE_INTEGER_MENU)
			v4l2_ctrl_new_std_menu(&ctx->ctrl_hdl,
					       cfg->ops, cfg->id, cfg->max,
					       cfg->menu_skip_mask, cfg->def);
		else if (cfg->type > V4L2_CTRL_COMPOUND_TYPES)
			v4l2_ctrl_new_custom(&ctx->ctrl_hdl, cfg, ctx);
		else
			v4l2_ctrl_new_std(&ctx->ctrl_hdl, cfg->ops, cfg->id,
					  cfg->min, cfg->max, cfg->step,
					  cfg->def);

		if (ctx->ctrl_hdl.error)
			return ctx->ctrl_hdl.error;
	}

	return 0;
}

int v4l2_m2m_codec_init_ctrls(struct v4l2_m2m_codec_ctx *ctx,
			      struct v4l2_m2m_codec_coded_fmt *fmts,
			      unsigned int nfmts,
			      const struct v4l2_ctrl_config *extra_ctrls,
			      unsigned int nextra_ctrls)
{
	unsigned int i, nctrls = nextra_ctrls;
	int ret;

	for (i = 0; i < nfmts; i++)
		nctrls += fmts[i].ctrls->nmandatory + fmts[i].ctrls->noptional;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, nctrls);

	for (i = 0; i < nfmts; i++) {
		ret = v4l2_m2m_codec_add_ctrls(ctx, fmts[i].ctrls->mandatory,
					       fmts[i].ctrls->nmandatory);
		if (ret)
			goto err_free_handler;

		ret = v4l2_m2m_codec_add_ctrls(ctx, fmts[i].ctrls->optional,
					       fmts[i].ctrls->noptional);
		if (ret)
			goto err_free_handler;
	}

	ret = v4l2_m2m_codec_add_ctrls(ctx, extra_ctrls, nextra_ctrls);
	if (ret)
		goto err_free_handler;

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_hdl);
	if (ret)
		goto err_free_handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	return ret;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_init_ctrls);

void v4l2_m2m_codec_cleanup_ctrls(struct v4l2_m2m_codec_ctx *ctx)
{
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_cleanup_ctrls);

void v4l2_m2m_codec_open(struct file *file,
			 struct v4l2_m2m_dev *m2m_dev,
			 struct v4l2_m2m_codec_ctx *ctx)
{
	ctx->m2m_dev = m2m_dev;

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_open);

void v4l2_m2m_codec_release(struct v4l2_m2m_codec_ctx *ctx)
{
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_release);

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

void v4l2_m2m_codec_job_finish(struct v4l2_m2m_codec_ctx *ctx,
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

	v4l2_m2m_job_finish(ctx->m2m_dev, ctx->fh.m2m_ctx);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_job_finish);

int v4l2_m2m_codec_request_validate(struct media_request *req)
{
	struct v4l2_m2m_codec_ctx *ctx;
	struct v4l2_ctrl_handler *hdl;
	struct vb2_buffer *vb;
	unsigned int count;
	unsigned int i;

	vb = vb2_request_get_buf(req, 0);
        if (!vb)
                return -ENOENT;

	ctx = vb2_get_drv_priv(vb->vb2_queue);
        if (!ctx)
                return -EINVAL;

	count = vb2_request_buffer_cnt(req);
	if (!count)
		return -ENOENT;
	else if (count > 1)
		return -EINVAL;

	hdl = v4l2_ctrl_request_hdl_find(req, &ctx->ctrl_hdl);
	if (!hdl)
		return -ENOENT;

	for (i = 0; i < ctx->coded_fmt->ctrls->nmandatory; i++) {
		u32 id = ctx->coded_fmt->ctrls->mandatory[i].id;
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_request_hdl_ctrl_find(hdl, id);
		if (!ctrl)
			return -ENOENT;
	}

	v4l2_ctrl_request_hdl_put(hdl);

	return vb2_request_validate(req);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_request_validate);
