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
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem-codec.h>

int v4l2_m2m_codec_init(struct v4l2_m2m_codec *codec,
			enum v4l2_m2m_codec_type type,
			struct v4l2_m2m_dev *m2m_dev,
			struct v4l2_device *v4l2_dev,
			const struct v4l2_m2m_codec_caps *caps,
			const struct v4l2_file_operations *fops,
			const struct v4l2_ioctl_ops *ioctl_ops,
			struct mutex *lock, const char *name, void *drvdata)
{
	struct video_device *vdev = v4l2_m2m_codec_to_vdev(codec);
	ssize_t ret;

	if (!codec || !caps || !m2m_dev ||
	    !caps->num_coded_fmts || !caps->num_decoded_fmts ||
	    !caps->coded_fmts || !caps->decoded_fmts)
		return -EINVAL;

	codec->type = type;
	codec->m2m_dev = m2m_dev;
	codec->caps = caps;
	vdev->lock = lock;
	vdev->v4l2_dev = v4l2_dev;
	vdev->fops = fops;
	vdev->release = video_device_release_empty;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_STREAMING;
	vdev->ioctl_ops = ioctl_ops;
	video_set_drvdata(vdev, drvdata);

	if (ioctl_ops->vidioc_g_fmt_vid_out_mplane)
		vdev->device_caps |= V4L2_CAP_VIDEO_M2M_MPLANE;
	else
		vdev->device_caps |= V4L2_CAP_VIDEO_M2M;

	ret = strscpy(vdev->name, name, sizeof(vdev->name));
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_init);

struct v4l2_m2m_codec_queue_init_ctx {
	struct v4l2_m2m_codec_ctx *ctx;
	int (*queue_init)(struct v4l2_m2m_codec_ctx *ctx,
			  struct vb2_queue *src_vq,
			  struct vb2_queue *dst_vq);
};

static int v4l2_m2m_codec_queue_init_wrapper(void *priv,
					     struct vb2_queue *src_vq,
					     struct vb2_queue *dst_vq)
{
	struct v4l2_m2m_codec_queue_init_ctx *qctx = priv;

	return qctx->queue_init(qctx->ctx, src_vq, dst_vq);
}

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

static void v4l2_m2m_codec_cleanup_ctrls(struct v4l2_m2m_codec_ctx *ctx)
{
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
}

static int v4l2_m2m_codec_init_ctrls(struct v4l2_m2m_codec_ctx *ctx,
				     const struct v4l2_ctrl_config *extra_ctrls,
				     unsigned int nextra_ctrls)
{
	const struct v4l2_m2m_codec_coded_fmt_desc *fmts;
	unsigned int i, nfmts, nctrls = nextra_ctrls;
	int ret;

	fmts = ctx->codec->caps->coded_fmts;
	nfmts = ctx->codec->caps->num_coded_fmts;
	for (i = 0; i < nfmts; i++)
		nctrls += fmts[i].ctrls->num_mandatory +
			  fmts[i].ctrls->num_optional;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, nctrls);

	for (i = 0; i < nfmts; i++) {
		ret = v4l2_m2m_codec_add_ctrls(ctx, fmts[i].ctrls->mandatory,
					       fmts[i].ctrls->num_mandatory);
		if (ret)
			goto err_free_handler;

		ret = v4l2_m2m_codec_add_ctrls(ctx, fmts[i].ctrls->optional,
					       fmts[i].ctrls->num_optional);
		if (ret)
			goto err_free_handler;
	}

	ret = v4l2_m2m_codec_add_ctrls(ctx, extra_ctrls, nextra_ctrls);
	if (ret)
		goto err_free_handler;

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_hdl);
	if (ret)
		goto err_free_handler;

	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	return ret;
}

int v4l2_m2m_codec_ctx_init(struct v4l2_m2m_codec_ctx *ctx, struct file *file,
			    struct v4l2_m2m_codec *codec,
			    const struct v4l2_ctrl_config *extra_ctrls,
			    unsigned int nextra_ctrls,
			    int (*queue_init)(struct v4l2_m2m_codec_ctx *ctx,
					      struct vb2_queue *src_vq,
					      struct vb2_queue *dst_vq))
{
	struct v4l2_m2m_codec_queue_init_ctx qctx = {
		.ctx = ctx,
		.queue_init = queue_init,
	};
	int ret;

	ctx->codec = codec;
	ret = v4l2_m2m_codec_init_ctrls(ctx, extra_ctrls, nextra_ctrls);
	if (ret)
		return ret;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(codec->m2m_dev, &qctx,
					    v4l2_m2m_codec_queue_init_wrapper);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_cleanup_ctrls;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);
	return 0;

err_cleanup_ctrls:
	v4l2_m2m_codec_cleanup_ctrls(ctx);
	return ret;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_ctx_init);

void v4l2_m2m_codec_ctx_cleanup(struct v4l2_m2m_codec_ctx *ctx)
{
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_m2m_codec_cleanup_ctrls(ctx);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_ctx_cleanup);

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

	v4l2_m2m_job_finish(ctx->codec->m2m_dev, ctx->fh.m2m_ctx);
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

	for (i = 0; i < ctx->coded_fmt_desc->ctrls->num_mandatory; i++) {
		u32 id = ctx->coded_fmt_desc->ctrls->mandatory[i].id;
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_request_hdl_ctrl_find(hdl, id);
		if (!ctrl)
			return -ENOENT;
	}

	v4l2_ctrl_request_hdl_put(hdl);

	return vb2_request_validate(req);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_request_validate);

const struct v4l2_m2m_codec_coded_fmt_desc *
v4l2_m2m_codec_find_coded_fmt_desc(struct v4l2_m2m_codec *codec, u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < codec->caps->num_coded_fmts; i++) {
		if (codec->caps->coded_fmts[i].fourcc == fourcc)
			return &codec->caps->coded_fmts[i];
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_find_coded_fmt_desc);

int v4l2_m2m_codec_enum_framesizes(struct file *file, void *priv,
				   struct v4l2_frmsizeenum *fsize)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_m2m_codec *codec = vdev_to_v4l2_m2m_codec(vdev);
	const struct v4l2_m2m_codec_coded_fmt_desc *fmt;

	if (fsize->index != 0)
		return -EINVAL;

	fmt = v4l2_m2m_codec_find_coded_fmt_desc(codec, fsize->pixel_format);
	if (!fmt)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->frmsize;

	return 0;

}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_enum_framesizes);

static int v4l2_m2m_codec_enum_coded_fmt(struct file *file, void *priv,
					 struct v4l2_fmtdesc *f)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_m2m_codec *codec = vdev_to_v4l2_m2m_codec(vdev);

	if (f->index >= codec->caps->num_coded_fmts)
		return -EINVAL;

	f->pixelformat = codec->caps->coded_fmts[f->index].fourcc;
	return 0;
}

static int v4l2_m2m_codec_enum_decoded_fmt(struct file *file, void *priv,
					   struct v4l2_fmtdesc *f)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_m2m_codec *codec = vdev_to_v4l2_m2m_codec(vdev);

	if (f->index >= codec->caps->num_decoded_fmts)
		return -EINVAL;

	f->pixelformat = codec->caps->decoded_fmts[f->index];
	return 0;
}

int v4l2_m2m_codec_enum_output_fmt(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (ctx->codec->type == V4L2_M2M_ENCODER)
		return v4l2_m2m_codec_enum_coded_fmt(file, priv, f);

	return v4l2_m2m_codec_enum_decoded_fmt(file, priv, f);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_enum_output_fmt);

int v4l2_m2m_codec_enum_capture_fmt(struct file *file, void *priv,
				    struct v4l2_fmtdesc *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (ctx->codec->type == V4L2_M2M_ENCODER)
		return v4l2_m2m_codec_enum_decoded_fmt(file, priv, f);

	return v4l2_m2m_codec_enum_coded_fmt(file, priv, f);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_enum_capture_fmt);

static int v4l2_m2m_codec_g_coded_fmt(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (V4L2_TYPE_IS_MULTIPLANAR(f->type))
		f->fmt.pix_mp = ctx->coded_fmt.mplane;
	else
		f->fmt.pix = ctx->coded_fmt.splane;

	return 0;
}

static int v4l2_m2m_codec_g_decoded_fmt(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (V4L2_TYPE_IS_MULTIPLANAR(f->type))
		f->fmt.pix_mp = ctx->decoded_fmt.mplane;
	else
		f->fmt.pix = ctx->decoded_fmt.splane;

	return 0;
}

int v4l2_m2m_codec_g_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (ctx->codec->type == V4L2_M2M_ENCODER)
		return v4l2_m2m_codec_g_decoded_fmt(file, priv, f);

	return v4l2_m2m_codec_g_coded_fmt(file, priv, f);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_g_output_fmt);

int v4l2_m2m_codec_g_capture_fmt(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (ctx->codec->type == V4L2_M2M_ENCODER)
		return v4l2_m2m_codec_g_coded_fmt(file, priv, f);

	return v4l2_m2m_codec_g_decoded_fmt(file, priv, f);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_g_capture_fmt);

static int v4l2_m2m_codec_s_fmt(struct file *file, void *priv,
				struct v4l2_format *f,
				int (*try_fmt)(struct file *, void *,
					       struct v4l2_format *))
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	struct vb2_queue *vq;
	int ret;

	if (!try_fmt)
		return -EINVAL;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = try_fmt(file, priv, f);
	if (ret)
		return ret;

	if (V4L2_TYPE_IS_OUTPUT(f->type) == 
	    (ctx->codec->type == V4L2_M2M_DECODER)) {
		const struct v4l2_m2m_codec_coded_fmt_desc *desc;
		u32 fourcc;

		if (!V4L2_TYPE_IS_MULTIPLANAR(f->type))
			fourcc = f->fmt.pix.pixelformat;
		else
			fourcc = f->fmt.pix_mp.pixelformat;

		desc = v4l2_m2m_codec_find_coded_fmt_desc(ctx->codec, fourcc);
		if (!desc)
			return -EINVAL;

		ctx->coded_fmt_desc = desc;
	}

	return 0;
}

int v4l2_m2m_codec_s_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	struct video_device *vfd = video_devdata(file);
	const struct v4l2_ioctl_ops *ops = vfd->ioctl_ops;
	union v4l2_m2m_codec_fmt *cap_fmt, *out_fmt;
	int ret;

	ret = v4l2_m2m_codec_s_fmt(file, priv, f,
				   V4L2_TYPE_IS_MULTIPLANAR(f->type) ?
				   ops->vidioc_try_fmt_vid_cap_mplane :
				   ops->vidioc_try_fmt_vid_cap);
	if (ret)
		return ret;

	if (ctx->codec->type == V4L2_M2M_DECODER) {
		out_fmt = &ctx->coded_fmt;
		cap_fmt = &ctx->decoded_fmt;
	} else {
		out_fmt = &ctx->decoded_fmt;
		cap_fmt = &ctx->coded_fmt;
	}

	/* Propagate colorspace information to capture. */
	if (V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
		out_fmt->mplane = f->fmt.pix_mp;
		cap_fmt->mplane.colorspace = f->fmt.pix_mp.colorspace;
		cap_fmt->mplane.xfer_func = f->fmt.pix_mp.xfer_func;
		cap_fmt->mplane.ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
		cap_fmt->mplane.quantization = f->fmt.pix_mp.quantization;
	} else {
		out_fmt->splane = f->fmt.pix;
		cap_fmt->splane.colorspace = f->fmt.pix.colorspace;
		cap_fmt->splane.xfer_func = f->fmt.pix.xfer_func;
		cap_fmt->splane.ycbcr_enc = f->fmt.pix.ycbcr_enc;
		cap_fmt->splane.quantization = f->fmt.pix.quantization;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_s_output_fmt);

int v4l2_m2m_codec_s_capture_fmt(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	struct video_device *vfd = video_devdata(file);
	const struct v4l2_ioctl_ops *ops = vfd->ioctl_ops;
	union v4l2_m2m_codec_fmt *cap_fmt, *out_fmt;
	int ret;

	ret = v4l2_m2m_codec_s_fmt(file, priv, f,
				   V4L2_TYPE_IS_MULTIPLANAR(f->type) ?
				   ops->vidioc_try_fmt_vid_out_mplane:
				   ops->vidioc_try_fmt_vid_out);
	if (ret)
		return ret;

	if (ctx->codec->type == V4L2_M2M_DECODER)
		cap_fmt = &ctx->decoded_fmt;
	else
		cap_fmt = &ctx->coded_fmt;

	if (V4L2_TYPE_IS_MULTIPLANAR(f->type))
		cap_fmt->mplane = f->fmt.pix_mp;
	else
		cap_fmt->splane = f->fmt.pix;

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_s_capture_fmt);

int v4l2_m2m_codec_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			       unsigned int *num_planes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vq);
	union v4l2_m2m_codec_fmt *fmt;
	int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type) == 
	    (ctx->codec->type == V4L2_M2M_DECODER))
		fmt = &ctx->coded_fmt;
	else
		fmt = &ctx->decoded_fmt;

	if (!V4L2_TYPE_IS_MULTIPLANAR(vq->type)) {
		if (*num_planes) {
			if (*num_planes != 1)
				return -EINVAL;

			if (sizes[0] < fmt->splane.sizeimage)
				return -EINVAL;
		} else {
			sizes[0] = fmt->splane.sizeimage;
		}

		return 0;
	}

	if (*num_planes) {
		if (*num_planes != fmt->mplane.num_planes)
			return -EINVAL;

		for (i = 0; i < fmt->mplane.num_planes; i++) {
			if (sizes[i] < fmt->mplane.plane_fmt[i].sizeimage)
				return -EINVAL;
		}

		return 0;
	}

	*num_planes = fmt->mplane.num_planes;
	for (i = 0; i < fmt->mplane.num_planes; i++)
		sizes[i] = fmt->mplane.plane_fmt[i].sizeimage;

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_queue_setup);

void v4l2_m2m_codec_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vq);

	while (true) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (!vbuf)
			break;

		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->ctrl_hdl);
		v4l2_m2m_buf_done(vbuf, state);
	}
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_queue_cleanup);

int v4l2_m2m_codec_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_buf_out_validate);

int v4l2_m2m_codec_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vq);
	union v4l2_m2m_codec_fmt *fmt;
	int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type) ==
	    (ctx->codec->type == V4L2_M2M_DECODER))
		fmt = &ctx->coded_fmt;
	else
		fmt = &ctx->decoded_fmt;

	if (!V4L2_TYPE_IS_MULTIPLANAR(vq->type)) {
		if (vb2_plane_size(vb, 0) < fmt->splane.sizeimage)
			return -EINVAL;

		vb2_set_plane_payload(vb, 0, fmt->splane.sizeimage);
		return 0;
	}

	for (i = 0; i < fmt->mplane.num_planes; ++i) {
		if (vb2_plane_size(vb, i) < fmt->mplane.plane_fmt[i].sizeimage)
			return -EINVAL;

		vb2_set_plane_payload(vb, i,
				      fmt->mplane.plane_fmt[i].sizeimage);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_buf_prepare);

void v4l2_m2m_codec_buf_queue(struct vb2_buffer *vb)
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_buf_queue);

void v4l2_m2m_codec_buf_request_complete(struct vb2_buffer *vb)
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_hdl);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_buf_request_complete);
