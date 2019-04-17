// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2010-2011 Samsung Electronics Co., Ltd.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-sg.h>

#include "rockchip_vpu.h"
#include "rockchip_vpu_hw.h"
#include "rockchip_vpu_v4l2.h"

const struct v4l2_ioctl_ops rockchip_vpu_enc_ioctl_ops = {
	.vidioc_querycap = rockchip_vpu_vidioc_querycap,
	.vidioc_enum_framesizes = rockchip_vpu_vidioc_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = rockchip_vpu_vidioc_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = rockchip_vpu_vidioc_try_fmt_out,
	.vidioc_s_fmt_vid_out_mplane = rockchip_vpu_vidioc_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = rockchip_vpu_vidioc_s_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = rockchip_vpu_vidioc_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = rockchip_vpu_vidioc_g_fmt_cap,
	.vidioc_enum_fmt_vid_out_mplane = rockchip_vpu_vidioc_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap_mplane = rockchip_vpu_vidioc_enum_fmt_cap,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static int
rockchip_vpu_queue_setup(struct vb2_queue *vq,
			 unsigned int *num_buffers,
			 unsigned int *num_planes,
			 unsigned int sizes[],
			 struct device *alloc_devs[])
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pixfmt;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		pixfmt = &ctx->dst_fmt;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		pixfmt = &ctx->src_fmt;
		break;
	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		return -EINVAL;
	}

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

static int rockchip_vpu_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vb2_queue *vq = vb->vb2_queue;
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pixfmt;
	unsigned int sz;
	int ret = 0;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		pixfmt = &ctx->dst_fmt;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		pixfmt = &ctx->src_fmt;

		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
		if (vbuf->field != V4L2_FIELD_NONE) {
			vpu_debug(4, "field %d not supported\n",
				  vbuf->field);
			return -EINVAL;
		}
		break;
	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		return -EINVAL;
	}

	for (i = 0; i < pixfmt->num_planes; ++i) {
		sz = pixfmt->plane_fmt[i].sizeimage;
		vpu_debug(4, "plane %d size: %ld, sizeimage: %u\n",
			  i, vb2_plane_size(vb, i), sz);
		if (vb2_plane_size(vb, i) < sz) {
			vpu_err("plane %d is too small\n", i);
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

static void rockchip_vpu_buf_queue(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int rockchip_vpu_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);
	enum rockchip_vpu_codec_mode codec_mode;
	int ret = 0;

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->sequence_out = 0;
	else
		ctx->sequence_cap = 0;

	/* Set codec_ops for the chosen destination format */
	codec_mode = ctx->vpu_dst_fmt->codec_mode;

	vpu_debug(4, "Codec mode = %d\n", codec_mode);
	ctx->codec_ops = &ctx->dev->variant->codec_ops[codec_mode];

	if (!V4L2_TYPE_IS_OUTPUT(q->type))
		if (ctx->codec_ops && ctx->codec_ops->init)
			ret = ctx->codec_ops->init(ctx);
	return ret;
}

static void rockchip_vpu_stop_streaming(struct vb2_queue *q)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	if (!V4L2_TYPE_IS_OUTPUT(q->type))
		if (ctx->codec_ops && ctx->codec_ops->exit)
			ctx->codec_ops->exit(ctx);

	/*
	 * The mem2mem framework calls v4l2_m2m_cancel_job before
	 * .stop_streaming, so there isn't any job running and
	 * it is safe to return all the buffers.
	 */
	for (;;) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req, &ctx->ctrl_handler);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static void rockchip_vpu_buf_request_complete(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_handler);
}

static int rockchip_vpu_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

const struct vb2_ops rockchip_vpu_enc_queue_ops = {
	.queue_setup = rockchip_vpu_queue_setup,
	.buf_prepare = rockchip_vpu_buf_prepare,
	.buf_queue = rockchip_vpu_buf_queue,
	.buf_out_validate = rockchip_vpu_buf_out_validate,
	.buf_request_complete = rockchip_vpu_buf_request_complete,
	.start_streaming = rockchip_vpu_start_streaming,
	.stop_streaming = rockchip_vpu_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};
