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
#include <media/v4l2-mem2mem-codec.h>

struct rkvdec_dev {
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct v4l2_m2m_codec codec;
	struct device *dev;
	struct clk_bulk_data *clocks;
	void __iomem *regs;
	struct mutex vdev_lock;
	struct delayed_work watchdog_work;
};

static inline struct rkvdec_dev *codec_to_rkvdec(struct v4l2_m2m_codec *codec)
{
	return container_of(codec, struct rkvdec_dev, codec);
}

struct rkvdec_ctx {
	struct v4l2_m2m_codec_ctx base;
	void *priv;
};

static inline struct rkvdec_ctx *
codec_ctx_to_rkvdec_ctx(struct v4l2_m2m_codec_ctx *ctx)
{
	return container_of(ctx, struct rkvdec_ctx, base);
}

static inline struct rkvdec_ctx *fh_to_rkvdec_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct rkvdec_ctx, base.fh);
}

struct rkvdec_aux_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

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

extern const struct v4l2_m2m_codec_coded_fmt_ops rkvdec_h264_fmt_ops;

#endif /* RKVDEC_H_ */
