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

const struct vb2_ops rockchip_vpu_enc_src_queue_ops = {
	.queue_setup = rockchip_vpu_src_queue_setup,
	.buf_prepare = rockchip_vpu_src_buf_prepare,
	.buf_queue = rockchip_vpu_buf_queue,
	.buf_out_validate = rockchip_vpu_buf_out_validate,
	.buf_request_complete = rockchip_vpu_buf_request_complete,
	.start_streaming = rockchip_vpu_start,
	.stop_streaming = rockchip_vpu_stop,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

const struct vb2_ops rockchip_vpu_enc_dst_queue_ops = {
	.queue_setup = rockchip_vpu_dst_queue_setup,
	.buf_prepare = rockchip_vpu_dst_buf_prepare,
	.buf_queue = rockchip_vpu_buf_queue,
	.buf_request_complete = rockchip_vpu_buf_request_complete,
	.start_streaming = rockchip_vpu_start,
	.stop_streaming = rockchip_vpu_stop,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};
