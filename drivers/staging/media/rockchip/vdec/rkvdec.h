/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hantro VPU codec driver
 *
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#ifndef RKVDEC_H_
#define RKVDEC_H_

#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/clk.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

struct rkvdec_ctrl_desc {
	u32 per_request : 1;
	u32 mandatory : 1;
	struct v4l2_ctrl_config cfg;
};

struct rkvdec_ctrls {
	const struct rkvdec_ctrl_desc *ctrls;
	unsigned int num_ctrls;
};

struct rkvdec_run {
	struct {
		struct vb2_v4l2_buffer *src;
		struct vb2_v4l2_buffer *dst;
	} bufs;
};

struct rkvdec_decoded_fmt_desc {
	u32 fourcc;
};

struct rkvdec_vp9_decoded_buffer_info {
	/* Info needed when the decoded frame serves as a reference frame. */
	struct v4l2_ctrl_vp9_frame_decode_params params;
	struct v4l2_ctrl_vp9_frame_ctx frame_context;

	u32 segmapid : 1;
};

struct rkvdec_decoded_buffer {
	/* Must be the first field in this struct. */
	struct v4l2_m2m_buffer base;

	union {
		struct rkvdec_vp9_decoded_buffer_info vp9;
	};
};

static inline struct rkvdec_decoded_buffer *
vb2_to_rkvdec_decoded_buf(struct vb2_buffer *buf)
{
	return container_of(buf, struct rkvdec_decoded_buffer,
			    base.vb.vb2_buf);
}

struct rkvdec_ctx;

struct rkvdec_coded_fmt_ops {
	int (*adjust_fmt)(struct rkvdec_ctx *ctx,
			  struct v4l2_format *f);
	int (*start)(struct rkvdec_ctx *ctx);
	void (*stop)(struct rkvdec_ctx *ctx);
	void (*run)(struct rkvdec_ctx *ctx);
	void (*done)(struct rkvdec_ctx *ctx, struct vb2_v4l2_buffer *src_buf,
		     struct vb2_v4l2_buffer *dst_buf,
		     enum vb2_buffer_state result);
};

struct rkvdec_coded_fmt_desc {
	u32 fourcc;
	struct v4l2_frmsize_stepwise frmsize;
	const struct rkvdec_ctrls *ctrls;
	const struct rkvdec_coded_fmt_ops *ops;
};

struct rkvdec_dev {
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct device *dev;
	struct clk_bulk_data *clocks;
	void __iomem *regs;
	struct mutex vdev_lock;
	struct delayed_work watchdog_work;
};

struct rkvdec_ctx {
	struct v4l2_fh fh;
	struct v4l2_format coded_fmt;
	struct v4l2_format decoded_fmt;
	const struct rkvdec_coded_fmt_desc *coded_fmt_desc;
	const struct rkvdec_decoded_fmt_desc *decoded_fmt_desc;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct rkvdec_dev *dev;
	void *priv;
};

static inline struct rkvdec_ctx *fh_to_rkvdec_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct rkvdec_ctx, fh);
}

struct rkvdec_aux_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

void rkvdec_run_preamble(struct rkvdec_ctx *ctx, struct rkvdec_run *run);
void rkvdec_run_postamble(struct rkvdec_ctx *ctx, struct rkvdec_run *run);

static inline void rkvdec_set_field(u32 *buf, u32 bit_offset, u8 len_in_bits,
				    u32 val)
{
	u32 word = bit_offset / 32;
	u32 bit = bit_offset % 32;

	if (len_in_bits + bit > 32) {
		u32 len1 = 32 - bit;
		u32 len2 = len_in_bits + bit - 32;

		buf[word] &= ~(((1 << len1) - 1) << bit);
		buf[word] |= val << bit;

		val >>= (32 - bit);
		buf[word + 1] &= ~((1 << len2) - 1);
		buf[word + 1] |= val;
	} else {
		buf[word] &= ~(((1 << len_in_bits) - 1) << bit);
		buf[word] |= val << bit;
	}
}

#define RKVDEC_FIELD(_word, _bit)		((32 * (_word)) + (_bit))

#define RKVDEC_SET_FIELD(_buf, _field, _val)				\
	rkvdec_set_field(_buf, _field##_OFF, _field##_LEN, _val)

extern const struct rkvdec_coded_fmt_ops rkvdec_h264_fmt_ops;
extern const struct rkvdec_coded_fmt_ops rkvdec_vp9_fmt_ops;

#endif /* RKVDEC_H_ */
