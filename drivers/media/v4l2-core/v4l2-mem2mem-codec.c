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
			const struct v4l2_m2m_codec_ops *ops,
			const struct v4l2_file_operations *fops,
			const struct v4l2_ioctl_ops *ioctl_ops,
			struct mutex *lock, const char *name, void *drvdata)
{
	struct video_device *vdev = v4l2_m2m_codec_to_vdev(codec);
	unsigned int i;
	ssize_t ret;

	if (!codec || !caps || !m2m_dev || !ops ||
	    !caps->num_coded_fmts || !caps->num_decoded_fmts ||
	    !caps->coded_fmts || !caps->decoded_fmts || !ops->queue_init)
		return -EINVAL;

	for (i = 0; i < caps->num_coded_fmts; i++) {
		if (!caps->coded_fmts[i].ops)
			return -EINVAL;
	}

	codec->type = type;
	codec->m2m_dev = m2m_dev;
	codec->caps = caps;
	codec->ops = ops;
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

static int v4l2_m2m_codec_add_ctrls(struct v4l2_m2m_codec_ctx *ctx,
				    const struct v4l2_m2m_codec_ctrls *ctrls)
{
	unsigned int i;

	if (ctrls->num_ctrls && !ctrls->ctrls)
		return -EINVAL;

	for (i = 0; i < ctrls->num_ctrls; i++) {
		const struct v4l2_ctrl_config *cfg = &ctrls->ctrls[i];

		v4l2_ctrl_new_custom(&ctx->ctrl_hdl, cfg, ctx);
		if (ctx->ctrl_hdl.error)
			return ctx->ctrl_hdl.error;
	}

	return 0;
}

static void v4l2_m2m_codec_cleanup_ctrls(struct v4l2_m2m_codec_ctx *ctx)
{
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
}

static int v4l2_m2m_codec_init_ctrls(struct v4l2_m2m_codec_ctx *ctx)
{
	const struct v4l2_m2m_codec_coded_fmt_desc *fmts;
	unsigned int i, nfmts, nctrls = 0;
	int ret;

	fmts = ctx->codec->caps->coded_fmts;
	nfmts = ctx->codec->caps->num_coded_fmts;
	for (i = 0; i < nfmts; i++)
		nctrls += fmts[i].ctrls->mandatory.num_ctrls +
			  fmts[i].ctrls->optional.num_ctrls;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, nctrls);

	for (i = 0; i < nfmts; i++) {
		if (!fmts[i].ctrls)
			continue;

		ret = v4l2_m2m_codec_add_ctrls(ctx, &fmts[i].ctrls->mandatory);
		if (ret)
			goto err_free_handler;

		ret = v4l2_m2m_codec_add_ctrls(ctx, &fmts[i].ctrls->optional);
		if (ret)
			goto err_free_handler;
	}

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_hdl);
	if (ret)
		goto err_free_handler;

	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	return ret;
}

static void v4l2_m2m_codec_reset_fmt(struct v4l2_m2m_codec_ctx *ctx,
				     struct v4l2_format *f, u32 fourcc)
{
	const struct v4l2_ioctl_ops *ops = ctx->codec->vdev.ioctl_ops;

	memset(f, 0, sizeof(*f));

	if (ops->vidioc_g_fmt_vid_cap_mplane) {
		f->fmt.pix_mp.pixelformat = fourcc;
	        f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	        f->fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG,
	        f->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	        f->fmt.pix_mp.quantization = V4L2_QUANTIZATION_DEFAULT;
	        f->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	} else {
		f->fmt.pix.pixelformat = fourcc;
	        f->fmt.pix.field = V4L2_FIELD_NONE;
	        f->fmt.pix.colorspace = V4L2_COLORSPACE_JPEG,
	        f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	        f->fmt.pix.quantization = V4L2_QUANTIZATION_DEFAULT;
	        f->fmt.pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	}
}

static void v4l2_m2m_codec_reset_coded_fmt(struct v4l2_m2m_codec_ctx *ctx)
{
	struct v4l2_m2m_codec *codec = ctx->codec;
	const struct v4l2_ioctl_ops *ops = codec->vdev.ioctl_ops;
	const struct v4l2_m2m_codec_coded_fmt_desc *desc;
	struct v4l2_format *f = &ctx->coded_fmt;

	desc = &ctx->codec->caps->coded_fmts[0];
	ctx->coded_fmt_desc = desc;
	v4l2_m2m_codec_reset_fmt(ctx, f, desc->fourcc);

	if (ops->vidioc_g_fmt_vid_cap_mplane) {
		struct v4l2_pix_format_mplane *fmt = &f->fmt.pix_mp;

		if (codec->type == V4L2_M2M_ENCODER)
			f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		else
			f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		if (desc->frmsize) {
			fmt->width = desc->frmsize->min_width;
			fmt->height = desc->frmsize->min_height;
		}

	} else {
		struct v4l2_pix_format *fmt = &f->fmt.pix;

		if (codec->type == V4L2_M2M_ENCODER)
			f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		else
			f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (desc->frmsize) {
			fmt->width = desc->frmsize->min_width;
			fmt->height = desc->frmsize->min_height;
		}
	}

	if (desc->ops->adjust_fmt)
		desc->ops->adjust_fmt(ctx, &ctx->coded_fmt);
}

void v4l2_m2m_codec_reset_decoded_fmt(struct v4l2_m2m_codec_ctx *ctx)
{
	struct v4l2_m2m_codec *codec = ctx->codec;
	const struct v4l2_ioctl_ops *ops = codec->vdev.ioctl_ops;
	const struct v4l2_m2m_codec_coded_fmt_desc *coded_desc;
	struct v4l2_format *f = &ctx->decoded_fmt;

	if (!ctx->coded_fmt_desc)
		v4l2_m2m_codec_reset_coded_fmt(ctx);

	coded_desc = ctx->coded_fmt_desc;
	v4l2_m2m_codec_reset_fmt(ctx, f, codec->caps->decoded_fmts[0].fourcc);
	if (ops->vidioc_g_fmt_vid_cap_mplane) {
		struct v4l2_pix_format_mplane *fmt = &f->fmt.pix_mp;

		if (codec->type == V4L2_M2M_ENCODER)
			f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		else
			f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

		if (coded_desc->frmsize) {
			fmt->width = coded_desc->frmsize->min_width;
			fmt->height = coded_desc->frmsize->min_height;
		}

		v4l2_fill_pixfmt_mp(fmt, fmt->pixelformat,
				    fmt->width, fmt->height);
	} else {
		struct v4l2_pix_format *fmt = &f->fmt.pix;

		if (codec->type == V4L2_M2M_ENCODER)
			f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		else
			f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

		if (coded_desc->frmsize) {
			fmt->width = coded_desc->frmsize->min_width;
			fmt->height = coded_desc->frmsize->min_height;
		}

		v4l2_fill_pixfmt(fmt, fmt->pixelformat,
				 fmt->width, fmt->height);
	}

	ctx->decoded_fmt_desc = &codec->caps->decoded_fmts[0];
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_reset_decoded_fmt);

static int v4l2_m2m_codec_queue_init(void *priv, struct vb2_queue *src_vq,
				     struct vb2_queue *dst_vq)
{
	struct v4l2_m2m_codec_ctx *ctx = priv;

	return ctx->codec->ops->queue_init(ctx, src_vq, dst_vq);
}

int v4l2_m2m_codec_ctx_init(struct v4l2_m2m_codec_ctx *ctx, struct file *file,
			    struct v4l2_m2m_codec *codec)
{
	int ret;

	ctx->codec = codec;
	ret = v4l2_m2m_codec_init_ctrls(ctx);
	if (ret)
		return ret;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(codec->m2m_dev, ctx,
					    v4l2_m2m_codec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_cleanup_ctrls;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	v4l2_m2m_codec_reset_coded_fmt(ctx);
	v4l2_m2m_codec_reset_decoded_fmt(ctx);
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
	const struct v4l2_m2m_codec_coded_fmt_ctrls *ctrls;
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

	ctrls = ctx->coded_fmt_desc->ctrls;
	for (i = 0; ctrls && i < ctrls->mandatory.num_ctrls; i++) {
		u32 id = ctrls->mandatory.ctrls[i].id;
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

	if (!fmt->frmsize)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = *fmt->frmsize;
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

	f->pixelformat = codec->caps->decoded_fmts[f->index].fourcc;
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

int v4l2_m2m_codec_g_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (ctx->codec->type == V4L2_M2M_ENCODER)
		*f = ctx->decoded_fmt;
	else
		*f = ctx->coded_fmt;

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_g_output_fmt);

int v4l2_m2m_codec_g_capture_fmt(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);

	if (ctx->codec->type == V4L2_M2M_ENCODER)
		*f = ctx->coded_fmt;
	else
		*f = ctx->decoded_fmt;

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_g_capture_fmt);

static void
v4l2_m2m_codec_apply_frmsize_constraints(struct v4l2_format *f,
				const struct v4l2_frmsize_stepwise *frmsize)
{
	u32 *width, *height;

	if (!frmsize)
		return;

	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
		width = &f->fmt.pix.width;
       		height = &f->fmt.pix.height;
	} else {
		width = &f->fmt.pix_mp.width;
       		height = &f->fmt.pix_mp.height;
	}

	v4l2_apply_frmsize_constraints(width, height, frmsize);
}

static int v4l2_m2m_codec_try_coded_fmt(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	const struct v4l2_m2m_codec_coded_fmt_desc *desc;
	struct v4l2_m2m_codec *codec = ctx->codec;
	u32 fourcc;
	int ret;

	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type))
		fourcc = f->fmt.pix.pixelformat;
	else
		fourcc = f->fmt.pix_mp.pixelformat;

	desc = v4l2_m2m_codec_find_coded_fmt_desc(codec, fourcc);
	if (!desc)
		return -EINVAL;

	v4l2_m2m_codec_apply_frmsize_constraints(f, desc->frmsize);

	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
		f->fmt.pix.field = V4L2_FIELD_NONE;
	} else {
		f->fmt.pix_mp.field = V4L2_FIELD_NONE;
		/* All coded formats are considered single planar for now. */
		f->fmt.pix_mp.num_planes = 1;
	}

	if (desc->ops->adjust_fmt) {
		ret = desc->ops->adjust_fmt(ctx, f);
		if (ret)
			return ret;
	}

	return 0;
}

static int v4l2_m2m_codec_try_decoded_fmt(struct file *file, void *priv,
					  struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	const struct v4l2_m2m_codec_coded_fmt_desc *coded_desc;
	struct v4l2_m2m_codec *codec = ctx->codec;
	unsigned int i;
	u32 fourcc;

	/*
	 * The codec context should point to a coded format desc, if the format
	 * on the coded end has not been set yet, it should point to the
	 * default value.
	 */
	coded_desc = ctx->coded_fmt_desc;
	if (WARN_ON(!coded_desc))
		return -EINVAL;

	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type))
		fourcc = f->fmt.pix.pixelformat;
	else
		fourcc = f->fmt.pix_mp.pixelformat;

	for (i = 0; i < codec->caps->num_decoded_fmts; i++) {
		if (codec->caps->decoded_fmts[i].fourcc == fourcc)
			break;
	}

	if (i == codec->caps->num_decoded_fmts)
		return -EINVAL;

	/* Always apply the frmsize constraint of the coded end. */
	v4l2_m2m_codec_apply_frmsize_constraints(f, coded_desc->frmsize);

	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
		v4l2_fill_pixfmt(&f->fmt.pix, fourcc, f->fmt.pix.width,
				 f->fmt.pix.height);
		f->fmt.pix.field = V4L2_FIELD_NONE;
	} else {
		v4l2_fill_pixfmt_mp(&f->fmt.pix_mp, fourcc, f->fmt.pix_mp.width,
				    f->fmt.pix_mp.height);
		f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	}

	return 0;
}

int v4l2_m2m_codec_try_output_fmt(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	
	if (ctx->codec->type == V4L2_M2M_ENCODER)
		return v4l2_m2m_codec_try_decoded_fmt(file, priv, f);

	return v4l2_m2m_codec_try_coded_fmt(file, priv, f);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_try_output_fmt);

int v4l2_m2m_codec_try_capture_fmt(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	
	if (ctx->codec->type == V4L2_M2M_ENCODER)
		return v4l2_m2m_codec_try_coded_fmt(file, priv, f);

	return v4l2_m2m_codec_try_decoded_fmt(file, priv, f);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_try_capture_fmt);

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
		struct v4l2_m2m_ctx *m2m_ctx = v4l2_m2m_codec_get_m2m_ctx(ctx);
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
		m2m_ctx->out_q_ctx.q.requires_requests = desc->requires_requests;
	}

	return 0;
}

int v4l2_m2m_codec_s_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_m2m_codec_ctx *ctx = fh_to_v4l2_m2m_codec_ctx(priv);
	struct video_device *vfd = video_devdata(file);
	const struct v4l2_ioctl_ops *ops = vfd->ioctl_ops;
	struct v4l2_format *cap_fmt;
	int ret;

	ret = v4l2_m2m_codec_s_fmt(file, priv, f,
				   V4L2_TYPE_IS_MULTIPLANAR(f->type) ?
				   ops->vidioc_try_fmt_vid_cap_mplane :
				   ops->vidioc_try_fmt_vid_cap);
	if (ret)
		return ret;

	if (ctx->codec->type == V4L2_M2M_DECODER) {
		ctx->coded_fmt = *f;
		cap_fmt = &ctx->decoded_fmt;
	} else {
		ctx->decoded_fmt = *f;
		cap_fmt = &ctx->coded_fmt;
	}

	/* Propagate colorspace information to capture. */
	if (V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
		cap_fmt->fmt.pix_mp.colorspace = f->fmt.pix_mp.colorspace;
		cap_fmt->fmt.pix_mp.xfer_func = f->fmt.pix_mp.xfer_func;
		cap_fmt->fmt.pix_mp.ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
		cap_fmt->fmt.pix_mp.quantization = f->fmt.pix_mp.quantization;
	} else {
		cap_fmt->fmt.pix.colorspace = f->fmt.pix.colorspace;
		cap_fmt->fmt.pix.xfer_func = f->fmt.pix.xfer_func;
		cap_fmt->fmt.pix.ycbcr_enc = f->fmt.pix.ycbcr_enc;
		cap_fmt->fmt.pix.quantization = f->fmt.pix.quantization;
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
	int ret;

	ret = v4l2_m2m_codec_s_fmt(file, priv, f,
				   V4L2_TYPE_IS_MULTIPLANAR(f->type) ?
				   ops->vidioc_try_fmt_vid_out_mplane:
				   ops->vidioc_try_fmt_vid_out);
	if (ret)
		return ret;

	if (ctx->codec->type == V4L2_M2M_DECODER)
		ctx->decoded_fmt = *f;
	else
		ctx->coded_fmt = *f;

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_s_capture_fmt);

int v4l2_m2m_codec_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			       unsigned int *num_planes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_format *f;
	int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type) == 
	    (ctx->codec->type == V4L2_M2M_DECODER))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	if (!V4L2_TYPE_IS_MULTIPLANAR(vq->type)) {
		if (*num_planes) {
			if (*num_planes != 1)
				return -EINVAL;

			if (sizes[0] < f->fmt.pix.sizeimage)
				return -EINVAL;
		} else {
			sizes[0] = f->fmt.pix.sizeimage;
		}

		return 0;
	}

	if (*num_planes) {
		if (*num_planes != f->fmt.pix_mp.num_planes)
			return -EINVAL;

		for (i = 0; i < f->fmt.pix_mp.num_planes; i++) {
			if (sizes[i] < f->fmt.pix_mp.plane_fmt[i].sizeimage)
				return -EINVAL;
		}

		return 0;
	}

	*num_planes = f->fmt.pix_mp.num_planes;
	for (i = 0; i < f->fmt.pix_mp.num_planes; i++)
		sizes[i] = f->fmt.pix_mp.plane_fmt[i].sizeimage;

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
	struct v4l2_format *f;
	int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type) ==
	    (ctx->codec->type == V4L2_M2M_DECODER))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	if (!V4L2_TYPE_IS_MULTIPLANAR(vq->type)) {
		if (vb2_plane_size(vb, 0) < f->fmt.pix.sizeimage)
			return -EINVAL;

		vb2_set_plane_payload(vb, 0, f->fmt.pix.sizeimage);
		return 0;
	}

	for (i = 0; i < f->fmt.pix_mp.num_planes; ++i) {
		if (vb2_plane_size(vb, i) < f->fmt.pix_mp.plane_fmt[i].sizeimage)
			return -EINVAL;

		vb2_set_plane_payload(vb, i,
				      f->fmt.pix_mp.plane_fmt[i].sizeimage);
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

int v4l2_m2m_codec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_m2m_codec *codec = ctx->codec;
	int ret;

	if ((codec->type == V4L2_M2M_DECODER) != V4L2_TYPE_IS_OUTPUT(q->type))
		return 0;

	desc = ctx->coded_fmt_desc;
	if (WARN_ON(!desc))
		return -EINVAL;

	if (desc->ops->start) {
		ret = desc->ops->start(ctx);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_start_streaming);

void v4l2_m2m_codec_stop_streaming(struct vb2_queue *q)
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	const struct v4l2_m2m_codec_coded_fmt_desc *desc;
	struct v4l2_m2m_codec *codec = ctx->codec;
	int ret;

	if ((codec->type == V4L2_M2M_DECODER) == V4L2_TYPE_IS_OUTPUT(q->type))
		return;

	desc = ctx->coded_fmt_desc;
	if (WARN_ON(!desc))
		return;

	if (desc->ops->stop)
		desc->ops->stop(ctx);
}
EXPORT_SYMBOL_GPL(v4l2_m2m_codec_stop_streaming);
