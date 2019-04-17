/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#ifndef ROCKCHIP_VPU_V4L2_H_
#define ROCKCHIP_VPU_V4L2_H_

#include "rockchip_vpu.h"

extern const struct v4l2_ioctl_ops rockchip_vpu_enc_ioctl_ops;
extern const struct vb2_ops rockchip_vpu_enc_src_queue_ops;
extern const struct vb2_ops rockchip_vpu_enc_dst_queue_ops;

void rockchip_vpu_reset_dst_fmt(struct video_device *vfd,
				struct rockchip_vpu_ctx *ctx);
void rockchip_vpu_reset_src_fmt(struct video_device *vfd,
				struct rockchip_vpu_ctx *ctx);
int rockchip_vpu_vidioc_querycap(struct file *file, void *priv,
				 struct v4l2_capability *cap);
int rockchip_vpu_vidioc_enum_framesizes(struct file *file, void *priv,
					struct v4l2_frmsizeenum *fsize);
int rockchip_vpu_vidioc_enum_fmt_cap(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f);
int rockchip_vpu_vidioc_enum_fmt_out(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f);
int rockchip_vpu_vidioc_try_fmt_cap(struct file *file, void *priv,
				    struct v4l2_format *f);
int rockchip_vpu_vidioc_try_fmt_out(struct file *file, void *priv,
				    struct v4l2_format *f);
int rockchip_vpu_vidioc_g_fmt_out(struct file *file, void *priv,
				  struct v4l2_format *f);
int rockchip_vpu_vidioc_g_fmt_cap(struct file *file, void *priv,
				  struct v4l2_format *f);
int rockchip_vpu_vidioc_s_fmt_out(struct file *file, void *priv,
				  struct v4l2_format *f);
int rockchip_vpu_vidioc_s_fmt_cap(struct file *file, void *priv,
				  struct v4l2_format *f);

void rockchip_vpu_buf_queue(struct vb2_buffer *vb);
int rockchip_vpu_src_queue_setup(struct vb2_queue *vq,
				 unsigned int *num_buffers,
				 unsigned int *num_planes,
				 unsigned int sizes[],
				 struct device *alloc_devs[]);
int rockchip_vpu_dst_queue_setup(struct vb2_queue *vq,
				 unsigned int *num_buffers,
				 unsigned int *num_planes,
				 unsigned int sizes[],
				 struct device *alloc_devs[]);
void rockchip_vpu_buf_request_complete(struct vb2_buffer *vb);
int rockchip_vpu_buf_out_validate(struct vb2_buffer *vb);
int rockchip_vpu_src_buf_prepare(struct vb2_buffer *vb);
int rockchip_vpu_dst_buf_prepare(struct vb2_buffer *vb);
int rockchip_vpu_start(struct vb2_queue *q, unsigned int count);
void rockchip_vpu_stop(struct vb2_queue *q);

#endif /* ROCKCHIP_VPU_V4L2_H_ */
