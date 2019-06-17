// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver
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

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_v4l2.h"

static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct hantro_dev *vpu = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, vpu->dev->driver->name, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform: %s",
		 vpu->dev->driver->name);
	return 0;
}

static int
vidioc_s_fmt_out_mplane(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct hantro_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_m2m_ctx *m2m_ctx = v4l2_m2m_codec_get_m2m_ctx(&ctx->base);
	int ret;

	if (!hantro_is_encoder_ctx(ctx)) {
		struct vb2_queue *peer_vq;

		/*
		 * Since format change on the OUTPUT queue will reset
		 * the CAPTURE queue, we can't allow doing so
		 * when the CAPTURE queue has buffers allocated.
		 */
		peer_vq = v4l2_m2m_get_vq(m2m_ctx,
					  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		if (vb2_is_busy(peer_vq))
			return -EBUSY;
	}

	ret = v4l2_m2m_codec_s_output_fmt(file, priv, f);
	if (ret)
		return ret;

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 * Note that hantro_reset_raw_fmt() also propagates size
	 * changes to the raw format.
	 */
	if (!hantro_is_encoder_ctx(ctx))
		v4l2_m2m_codec_reset_decoded_fmt(&ctx->base);

	vpu_debug(0, "OUTPUT codec mode: %d\n", pix_mp->pixelformat);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);
	return 0;
}

static int vidioc_s_fmt_cap_mplane(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct hantro_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_m2m_ctx *m2m_ctx = v4l2_m2m_codec_get_m2m_ctx(&ctx->base);
	int ret;

	if (hantro_is_encoder_ctx(ctx)) {
		struct v4l2_pix_format_mplane *old_pix_mp;
		struct vb2_queue *peer_vq;

		/*
		 * Since format change on the CAPTURE queue will reset
		 * the OUTPUT queue, we can't allow doing so
		 * when the OUTPUT queue has buffers allocated.
		 */
		old_pix_mp = &ctx->base.coded_fmt.fmt.pix_mp;
		peer_vq = v4l2_m2m_get_vq(m2m_ctx,
					  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (vb2_is_busy(peer_vq) &&
		    (pix_mp->pixelformat != old_pix_mp->pixelformat ||
		     pix_mp->height != old_pix_mp->height ||
		     pix_mp->width != old_pix_mp->width))
			return -EBUSY;
	}

	ret = v4l2_m2m_codec_s_capture_fmt(file, priv, f);
	if (ret)
		return ret;

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 * Note that hantro_reset_raw_fmt() also propagates size
	 * changes to the raw format.
	 */
	if (hantro_is_encoder_ctx(ctx))
		v4l2_m2m_codec_reset_decoded_fmt(&ctx->base);

	vpu_debug(0, "CAPTURE codec mode: %d\n", pix_mp->pixelformat);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);

	return 0;
}

const struct v4l2_ioctl_ops hantro_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_framesizes = v4l2_m2m_codec_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = v4l2_m2m_codec_try_capture_fmt,
	.vidioc_try_fmt_vid_out_mplane = v4l2_m2m_codec_try_output_fmt,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt_out_mplane,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_cap_mplane,
	.vidioc_g_fmt_vid_out_mplane = v4l2_m2m_codec_g_output_fmt,
	.vidioc_g_fmt_vid_cap_mplane = v4l2_m2m_codec_g_capture_fmt,
	.vidioc_enum_fmt_vid_out = v4l2_m2m_codec_enum_output_fmt,
	.vidioc_enum_fmt_vid_cap = v4l2_m2m_codec_enum_capture_fmt,

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

const struct vb2_ops hantro_queue_ops = {
	.queue_setup = v4l2_m2m_codec_queue_setup,
	.buf_prepare = v4l2_m2m_codec_buf_prepare,
	.buf_queue = v4l2_m2m_codec_buf_queue,
	.buf_out_validate = v4l2_m2m_codec_buf_out_validate,
	.buf_request_complete = v4l2_m2m_codec_buf_request_complete,
	.start_streaming = v4l2_m2m_codec_start_streaming,
	.stop_streaming = v4l2_m2m_codec_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};
