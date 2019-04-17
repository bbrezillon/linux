// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec common vidioc_ helpers
 *
 * Copyright 2019 Collabora Ltd.
 *
 * Author: Ezequiel Garcia <ezequiel.garcia@collabora.com>
 */

#include <media/v4l2-mem2mem.h>

#include "rockchip_vpu_hw.h"
#include "rockchip_vpu_v4l2.h"
#include "rockchip_vpu.h"

static const struct rockchip_vpu_fmt *
rockchip_vpu_get_formats(struct video_device *vfd,
		   struct rockchip_vpu_dev *dev,
		   unsigned int *num_fmts)
{
	const struct rockchip_vpu_fmt *formats;

	formats = dev->variant->enc_fmts;
	*num_fmts = dev->variant->num_enc_fmts;

	return formats;
}

static const struct rockchip_vpu_fmt *
rockchip_vpu_find_format(const struct rockchip_vpu_fmt *formats,
			 unsigned int num_fmts, u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < num_fmts; i++)
		if (formats[i].fourcc == fourcc)
			return &formats[i];
	return NULL;
}

static const struct rockchip_vpu_fmt *
rockchip_vpu_get_default_fmt(const struct rockchip_vpu_fmt *formats,
			     unsigned int num_fmts, bool bitstream)
{
	unsigned int i;

	for (i = 0; i < num_fmts; i++) {
		if (bitstream == (formats[i].codec_mode != RK_VPU_MODE_NONE))
			return &formats[i];
	}
	return NULL;
}

static void rockchip_vpu_reset_fmt(struct v4l2_pix_format_mplane *fmt,
				   const struct rockchip_vpu_fmt *vpu_fmt,
				   bool coded)
{
	unsigned int width, height;

	memset(fmt, 0, sizeof(*fmt));

	width = clamp(fmt->width, vpu_fmt->frmsize.min_width,
		      vpu_fmt->frmsize.max_width);
	height = clamp(fmt->height, vpu_fmt->frmsize.min_height,
		       vpu_fmt->frmsize.max_height);
	fmt->pixelformat = vpu_fmt->fourcc;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_JPEG,
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;

	if (coded) {
		fmt->num_planes = 1;
		fmt->width = width;
		fmt->height = height;
		fmt->plane_fmt[0].sizeimage = vpu_fmt->header_size +
			fmt->width * fmt->height * vpu_fmt->max_depth;
	} else {
		v4l2_fill_pixfmt_mp(fmt, vpu_fmt->fourcc, width, height);
	}
}

void rockchip_vpu_reset_dst_fmt(struct video_device *vfd,
				struct rockchip_vpu_ctx *ctx)
{
	const struct rockchip_vpu_fmt *formats;
	unsigned int num_fmts;

	formats = rockchip_vpu_get_formats(vfd, ctx->dev, &num_fmts);
	ctx->vpu_dst_fmt = rockchip_vpu_get_default_fmt(formats, num_fmts,
							true);
	rockchip_vpu_reset_fmt(&ctx->dst_fmt, ctx->vpu_dst_fmt, true);
}

void rockchip_vpu_reset_src_fmt(struct video_device *vfd,
			  struct rockchip_vpu_ctx *ctx)
{
	const struct rockchip_vpu_fmt *formats;
	unsigned int num_fmts;

	formats = rockchip_vpu_get_formats(vfd, ctx->dev, &num_fmts);
	ctx->vpu_src_fmt = rockchip_vpu_get_default_fmt(formats, num_fmts,
							false);
	rockchip_vpu_reset_fmt(&ctx->src_fmt, ctx->vpu_src_fmt, false);
}

int rockchip_vpu_vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct video_device *vfd = video_devdata(file);
	struct rockchip_vpu_dev *vpu = video_drvdata(file);

	strscpy(cap->driver, vpu->dev->driver->name, sizeof(cap->driver));
	strscpy(cap->card, vfd->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform: %s",
		 vpu->dev->driver->name);
	return 0;
}

int rockchip_vpu_vidioc_enum_framesizes(struct file *file, void *priv,
					struct v4l2_frmsizeenum *fsize)
{
	struct video_device *vfd = video_devdata(file);
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct rockchip_vpu_dev *dev = ctx->dev;
	const struct rockchip_vpu_fmt *formats, *fmt;
	unsigned int num_fmts;

	if (fsize->index != 0) {
		vpu_debug(0, "invalid frame size index (expected 0, got %d)\n",
			  fsize->index);
		return -EINVAL;
	}

	formats = rockchip_vpu_get_formats(vfd, dev, &num_fmts);
	fmt = rockchip_vpu_find_format(formats, num_fmts, fsize->pixel_format);
	if (!fmt) {
		vpu_debug(0, "unsupported bitstream format (%08x)\n",
			  fsize->pixel_format);
		return -EINVAL;
	}

	/* This only makes sense for coded formats */
	if (fmt->codec_mode == RK_VPU_MODE_NONE)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->frmsize;

	return 0;
}

static int rockchip_vpu_vidioc_enum_fmt(struct file *file, void *priv,
					struct v4l2_fmtdesc *f, bool capture)

{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	struct video_device *vfd = video_devdata(file);
	const struct rockchip_vpu_fmt *fmt, *formats;
	unsigned int num_fmts, i, j = 0;

	formats = rockchip_vpu_get_formats(vfd, dev, &num_fmts);
	for (i = 0; i < num_fmts; i++) {
		bool mode_none = formats[i].codec_mode == RK_VPU_MODE_NONE;

		/*
		 * When dealing with an encoder:
		 *  - on the capture side we want to skip all MODE_NONE
		 *    formats.
		 *  - on the output side we want to skip all formats that are
		 *    not MODE_NONE.
		 */
		if (capture != mode_none)
			continue;
		if (j == f->index) {
			fmt = &formats[i];
			f->pixelformat = fmt->fourcc;
			return 0;
		}
		++j;
	}
	return -EINVAL;
}

int rockchip_vpu_vidioc_enum_fmt_cap(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f)
{
	return rockchip_vpu_vidioc_enum_fmt(file, priv, f, true);
}

int rockchip_vpu_vidioc_enum_fmt_out(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f)
{
	return rockchip_vpu_vidioc_enum_fmt(file, priv, f, false);
}

static int rockchip_vpu_vidioc_try_fmt(struct file *file, void *priv,
				       struct v4l2_format *f, bool coded)
{
	struct video_device *vfd = video_devdata(file);
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct rockchip_vpu_dev *dev = ctx->dev;
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *formats, *fmt, *vpu_fmt;
	unsigned int num_fmts;

	vpu_debug(4, "trying format %c%c%c%c\n",
		  (pix_mp->pixelformat & 0x7f),
		  (pix_mp->pixelformat >> 8) & 0x7f,
		  (pix_mp->pixelformat >> 16) & 0x7f,
		  (pix_mp->pixelformat >> 24) & 0x7f);

	formats = rockchip_vpu_get_formats(vfd, dev, &num_fmts);
	fmt = rockchip_vpu_find_format(formats, num_fmts, pix_mp->pixelformat);
	if (!fmt) {
		fmt = rockchip_vpu_get_default_fmt(formats, num_fmts, coded);
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	if (coded) {
		pix_mp->num_planes = 1;
		vpu_fmt = fmt;
	} else {
		vpu_fmt = ctx->vpu_dst_fmt;
	}

	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->width = clamp(pix_mp->width,
			      vpu_fmt->frmsize.min_width,
			      vpu_fmt->frmsize.max_width);
	pix_mp->height = clamp(pix_mp->height,
			       vpu_fmt->frmsize.min_height,
			       vpu_fmt->frmsize.max_height);

	/* Round up to macroblocks. */
	pix_mp->width = round_up(pix_mp->width, vpu_fmt->frmsize.step_width);
	pix_mp->height = round_up(pix_mp->height, vpu_fmt->frmsize.step_height);

	if (!coded) {
		/* Fill remaining fields */
		v4l2_fill_pixfmt_mp(pix_mp, fmt->fourcc, pix_mp->width,
				    pix_mp->height);
	} else if (!pix_mp->plane_fmt[0].sizeimage) {
		/*
		 * For coded formats the application can specify
		 * sizeimage. If the application passes a zero sizeimage,
		 * let's default to the maximum frame size.
		 */
		pix_mp->plane_fmt[0].sizeimage = fmt->header_size +
			pix_mp->width * pix_mp->height * fmt->max_depth;
	}

	return 0;
}

int rockchip_vpu_vidioc_try_fmt_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	return rockchip_vpu_vidioc_try_fmt(file, priv, f, true);
}

int rockchip_vpu_vidioc_try_fmt_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	return rockchip_vpu_vidioc_try_fmt(file, priv, f, false);
}

int rockchip_vpu_vidioc_g_fmt_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->src_fmt;

	return 0;
}

int rockchip_vpu_vidioc_g_fmt_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->dst_fmt;

	return 0;
}

int rockchip_vpu_vidioc_s_fmt_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct video_device *vfd = video_devdata(file);
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct rockchip_vpu_dev *vpu = ctx->dev;
	const struct rockchip_vpu_fmt *formats;
	unsigned int num_fmts;
	struct vb2_queue *vq;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	ret = rockchip_vpu_vidioc_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	formats = rockchip_vpu_get_formats(vfd, vpu, &num_fmts);
	ctx->vpu_src_fmt = rockchip_vpu_find_format(formats, num_fmts,
					      pix_mp->pixelformat);
	ctx->src_fmt = *pix_mp;

	/* Propagate to the CAPTURE format */
	ctx->dst_fmt.colorspace = pix_mp->colorspace;
	ctx->dst_fmt.ycbcr_enc = pix_mp->ycbcr_enc;
	ctx->dst_fmt.xfer_func = pix_mp->xfer_func;
	ctx->dst_fmt.quantization = pix_mp->quantization;
	ctx->dst_fmt.width = pix_mp->width;
	ctx->dst_fmt.height = pix_mp->height;

	vpu_debug(0, "OUTPUT codec mode: %d\n", ctx->vpu_src_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);
	return 0;
}

int rockchip_vpu_vidioc_s_fmt_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct video_device *vfd = video_devdata(file);
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct rockchip_vpu_dev *vpu = ctx->dev;
	const struct rockchip_vpu_fmt *formats;
	struct vb2_queue *vq, *peer_vq;
	unsigned int num_fmts;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	/*
	 * Since format change on the CAPTURE queue will reset
	 * the OUTPUT queue, we can't allow doing so
	 * when the OUTPUT queue has buffers allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
				  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (vb2_is_busy(peer_vq) &&
	    (pix_mp->pixelformat != ctx->dst_fmt.pixelformat ||
	     pix_mp->height != ctx->dst_fmt.height ||
	     pix_mp->width != ctx->dst_fmt.width))
		return -EBUSY;

	ret = rockchip_vpu_vidioc_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	formats = rockchip_vpu_get_formats(vfd, vpu, &num_fmts);
	ctx->vpu_dst_fmt = rockchip_vpu_find_format(formats, num_fmts,
					      pix_mp->pixelformat);
	ctx->dst_fmt = *pix_mp;

	vpu_debug(0, "CAPTURE codec mode: %d\n", ctx->vpu_dst_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 */
	rockchip_vpu_reset_src_fmt(vfd, ctx);
	return 0;
}

static int
rockchip_vpu_buf_plane_check(struct vb2_buffer *vb,
			     const struct rockchip_vpu_fmt *vpu_fmt,
			     struct v4l2_pix_format_mplane *pixfmt)
{
	unsigned int sz;
	int i;

	for (i = 0; i < pixfmt->num_planes; ++i) {
		sz = pixfmt->plane_fmt[i].sizeimage;
		vpu_debug(4, "plane %d size: %ld, sizeimage: %u\n",
			  i, vb2_plane_size(vb, i), sz);
		if (vb2_plane_size(vb, i) < sz) {
			vpu_err("plane %d is too small for output\n", i);
			return -EINVAL;
		}
	}
	return 0;
}

static int
rockchip_vpu_queue_setup(struct v4l2_pix_format_mplane *pixfmt,
			 unsigned int *num_planes,
			 unsigned int sizes[])
{
	int i;

	if (*num_planes) {
		if (*num_planes != pixfmt->num_planes)
			return -EINVAL;
		for (i = 0; i < pixfmt->num_planes; ++i)
			if (sizes[i] < pixfmt->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}

	*num_planes = pixfmt->num_planes;
	for (i = 0; i < pixfmt->num_planes; ++i)
		sizes[i] = pixfmt->plane_fmt[i].sizeimage;
	return 0;
}

static void
rockchip_vpu_return_bufs(struct vb2_queue *q,
			 struct vb2_v4l2_buffer *(*buf_remove)(struct v4l2_m2m_ctx *m2m_ctx))
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	for (;;) {
		struct vb2_v4l2_buffer *vbuf;

		vbuf = buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req, &ctx->ctrl_handler);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

void rockchip_vpu_buf_queue(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

int rockchip_vpu_src_queue_setup(struct vb2_queue *vq,
				 unsigned int *num_buffers,
				 unsigned int *num_planes,
				 unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);

	return rockchip_vpu_queue_setup(&ctx->src_fmt,
					num_planes, sizes);
}

int rockchip_vpu_dst_queue_setup(struct vb2_queue *vq,
				 unsigned int *num_buffers,
				 unsigned int *num_planes,
				 unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);

	return rockchip_vpu_queue_setup(&ctx->dst_fmt,
					num_planes, sizes);
}

void rockchip_vpu_buf_request_complete(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_handler);
}

int rockchip_vpu_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

int rockchip_vpu_src_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);

	return rockchip_vpu_buf_plane_check(vb, ctx->vpu_src_fmt,
					    &ctx->src_fmt);
}

int rockchip_vpu_dst_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);

	return rockchip_vpu_buf_plane_check(vb, ctx->vpu_dst_fmt,
					    &ctx->dst_fmt);
}

int rockchip_vpu_start(struct vb2_queue *q, unsigned int count)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);
	enum rockchip_vpu_codec_mode codec_mode;
	int ret = 0;

	if (q->ops == &rockchip_vpu_enc_dst_queue_ops) {
		ctx->sequence_out = 0;
		codec_mode = ctx->vpu_src_fmt->codec_mode;

		vpu_debug(4, "Codec mode = %d\n", codec_mode);
		ctx->codec_ops = &ctx->dev->variant->codec_ops[codec_mode];
		if (ctx->codec_ops && ctx->codec_ops->init)
			ret = ctx->codec_ops->init(ctx);
	} else {
		ctx->sequence_cap = 0;
	}

	return ret;
}

void rockchip_vpu_stop(struct vb2_queue *q)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	if (q->ops == &rockchip_vpu_enc_dst_queue_ops) {
		if (ctx->codec_ops && ctx->codec_ops->exit)
			ctx->codec_ops->exit(ctx);

		/*
		 * The mem2mem framework calls v4l2_m2m_cancel_job before
		 * .stop_streaming, so there isn't any job running and
		 * it is safe to return all the buffers.
		 */
		rockchip_vpu_return_bufs(q, v4l2_m2m_src_buf_remove);
	} else {
		rockchip_vpu_return_bufs(q, v4l2_m2m_dst_buf_remove);
	}
}
