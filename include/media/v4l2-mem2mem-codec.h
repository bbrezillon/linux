/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Memory-to-memory codec framework for Video for Linux 2.
 *
 * Helper functions for codec devices that use memory buffers for both source
 * and destination.
 *
 * Copyright (c) 2019 Collabora Ltd.
 *
 * Author:
 *	Boris Brezillon <boris.brezillon@collabora.com>
 */

#ifndef _MEDIA_V4L2_MEM2MEM_CODEC_H
#define _MEDIA_V4L2_MEM2MEM_CODEC_H

#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>

struct v4l2_m2m_codec_ctrls {
	const struct v4l2_ctrl_config *mandatory;
	unsigned int num_mandatory;
	const struct v4l2_ctrl_config *optional;
	unsigned int num_optional;
};

struct v4l2_m2m_codec_coded_fmt_desc {
	u32 fourcc;
	struct v4l2_frmsize_stepwise frmsize;
	const struct v4l2_m2m_codec_ctrls *ctrls;
	const void *priv;
};

struct v4l2_m2m_codec_caps {
	const struct v4l2_m2m_codec_coded_fmt_desc *coded_fmts;
	unsigned int num_coded_fmts;
	u32 *decoded_fmts;
	unsigned int num_decoded_fmts;
};

enum v4l2_m2m_codec_type {
	V4L2_M2M_ENCODER,
	V4L2_M2M_DECODER,
};

struct v4l2_m2m_codec {
	struct video_device vdev;
	enum v4l2_m2m_codec_type type;
	struct v4l2_m2m_dev *m2m_dev;
	const struct v4l2_m2m_codec_caps *caps;
};

static inline struct v4l2_m2m_codec *
vdev_to_v4l2_m2m_codec(struct video_device *vdev)
{
	return container_of(vdev, struct v4l2_m2m_codec, vdev);
}

static inline struct video_device *
v4l2_m2m_codec_to_vdev(struct v4l2_m2m_codec *codec)
{
	return &codec->vdev;
}

static inline enum v4l2_m2m_codec_type
v4l2_m2m_codec_get_type(const struct v4l2_m2m_codec *codec)
{
	return codec->type;
}

union v4l2_m2m_codec_fmt {
	struct v4l2_pix_format splane;
	struct v4l2_pix_format_mplane mplane;
};

struct v4l2_m2m_codec_ctx {
	struct v4l2_fh fh;
	union v4l2_m2m_codec_fmt coded_fmt;
	union v4l2_m2m_codec_fmt decoded_fmt;
	const struct v4l2_m2m_codec_coded_fmt_desc *coded_fmt_desc;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct v4l2_m2m_codec *codec;
};

static inline struct v4l2_m2m_codec_ctx *
fh_to_v4l2_m2m_codec_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct v4l2_m2m_codec_ctx, fh);
}

static inline struct v4l2_m2m_codec_ctx *
file_to_v4l2_m2m_codec_ctx(struct file *file)
{
	return fh_to_v4l2_m2m_codec_ctx(file->private_data);
}

static inline struct v4l2_m2m_ctx *
v4l2_m2m_codec_get_m2m_ctx(struct v4l2_m2m_codec_ctx *ctx)
{
	return ctx->fh.m2m_ctx;
}

struct v4l2_m2m_codec_run {
	struct {
		struct vb2_v4l2_buffer *src;
		struct vb2_v4l2_buffer *dst;
	} bufs;
};

int v4l2_m2m_codec_init(struct v4l2_m2m_codec *codec,
			enum v4l2_m2m_codec_type type,
			struct v4l2_m2m_dev *m2m_dev,
			struct v4l2_device *v4l2_dev,
			const struct v4l2_m2m_codec_caps *caps,
			const struct v4l2_file_operations *fops,
			const struct v4l2_ioctl_ops *ioctl_ops,
			struct mutex *lock, const char *name, void *drvdata);
int v4l2_m2m_codec_ctx_init(struct v4l2_m2m_codec_ctx *ctx, struct file *file,
			    struct v4l2_m2m_codec *codec,
			    const struct v4l2_ctrl_config *extra_ctrls,
			    unsigned int nextra_ctrls,
			    int (*queue_init)(struct v4l2_m2m_codec_ctx *ctx,
					      struct vb2_queue *src_vq,
					      struct vb2_queue *dst_vq));
void v4l2_m2m_codec_ctx_cleanup(struct v4l2_m2m_codec_ctx *ctx);
void v4l2_m2m_codec_run_preamble(struct v4l2_m2m_codec_ctx *ctx,
				 struct v4l2_m2m_codec_run *run);
void v4l2_m2m_codec_run_postamble(struct v4l2_m2m_codec_ctx *ctx,
				  struct v4l2_m2m_codec_run *run);
void v4l2_m2m_codec_job_finish(struct v4l2_m2m_codec_ctx *ctx,
			       enum vb2_buffer_state state);

const struct v4l2_m2m_codec_coded_fmt_desc *
v4l2_m2m_codec_find_coded_fmt_desc(struct v4l2_m2m_codec *codec, u32 fourcc);
int v4l2_m2m_codec_enum_framesizes(struct file *file, void *priv,
				   struct v4l2_frmsizeenum *fsize);
int v4l2_m2m_codec_enum_output_fmt(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f);
int v4l2_m2m_codec_enum_capture_fmt(struct file *file, void *priv,
				    struct v4l2_fmtdesc *f);
int v4l2_m2m_codec_g_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f);
int v4l2_m2m_codec_g_capture_fmt(struct file *file, void *priv,
				 struct v4l2_format *f);
int v4l2_m2m_codec_s_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f);
int v4l2_m2m_codec_s_capture_fmt(struct file *file, void *priv,
				 struct v4l2_format *f);

int v4l2_m2m_codec_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			       unsigned int *num_planes, unsigned int sizes[],
			       struct device *alloc_devs[]);
void v4l2_m2m_codec_queue_cleanup(struct vb2_queue *vq, u32 state);
int v4l2_m2m_codec_buf_out_validate(struct vb2_buffer *vb);
int v4l2_m2m_codec_buf_prepare(struct vb2_buffer *vb);
void v4l2_m2m_codec_buf_queue(struct vb2_buffer *vb);
void v4l2_m2m_codec_buf_request_complete(struct vb2_buffer *vb);

int v4l2_m2m_codec_request_validate(struct media_request *req);

#endif /* _MEDIA_V4L2_MEM2MEM_CODEC_H */
