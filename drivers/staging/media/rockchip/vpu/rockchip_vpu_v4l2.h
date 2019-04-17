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
extern const struct vb2_ops rockchip_vpu_enc_queue_ops;

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

#endif /* ROCKCHIP_VPU_V4L2_H_ */
