// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VP8 codec driver
 *
 * Copyright (C) 2019 Rockchip Electronics Co., Ltd.
 *	ZhiChao Yu <zhichao.yu@rock-chips.com>
 *
 * Copyright (C) 2019 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 */

#include <media/v4l2-mem2mem.h>
#include <media/vp8-ctrls.h>

#include "hantro_hw.h"
#include "hantro.h"
#include "hantro_g1_regs.h"

#define DEC_8190_ALIGN_MASK	0x07U

struct vp8_dec_reg {
	u32 base;
	u32 shift;
	u32 mask;
};

/* dct partition base address regs */
static const struct vp8_dec_reg vp8_dec_dct_base[8] = {
	{ G1_REG_ADDR_STR, 0, 0xffffffff },
	{ G1_REG_ADDR_REF(8), 0, 0xffffffff },
	{ G1_REG_ADDR_REF(9), 0, 0xffffffff },
	{ G1_REG_ADDR_REF(10), 0, 0xffffffff },
	{ G1_REG_ADDR_REF(11), 0, 0xffffffff },
	{ G1_REG_ADDR_REF(12), 0, 0xffffffff },
	{ G1_REG_ADDR_REF(14), 0, 0xffffffff },
	{ G1_REG_ADDR_REF(15), 0, 0xffffffff },
};

/* loop filter level regs */
static const struct vp8_dec_reg vp8_dec_lf_level[4] = {
	{ G1_REG_REF_PIC(2), 18, 0x3f },
	{ G1_REG_REF_PIC(2), 12, 0x3f },
	{ G1_REG_REF_PIC(2), 6, 0x3f },
	{ G1_REG_REF_PIC(2), 0, 0x3f },
};

/* macroblock loop filter level adjustment regs */
static const struct vp8_dec_reg vp8_dec_mb_adj[4] = {
	{ G1_REG_REF_PIC(0), 21, 0x7f },
	{ G1_REG_REF_PIC(0), 14, 0x7f },
	{ G1_REG_REF_PIC(0), 7, 0x7f },
	{ G1_REG_REF_PIC(0), 0, 0x7f },
};

/* reference frame adjustment regs */
static const struct vp8_dec_reg vp8_dec_ref_adj[4] = {
	{ G1_REG_REF_PIC(1), 21, 0x7f },
	{ G1_REG_REF_PIC(1), 14, 0x7f },
	{ G1_REG_REF_PIC(1), 7, 0x7f },
	{ G1_REG_REF_PIC(1), 0, 0x7f },
};

/* quantizer regs */
static const struct vp8_dec_reg vp8_dec_quant[4] = {
	{ G1_REG_REF_PIC(3), 11, 0x7ff },
	{ G1_REG_REF_PIC(3), 0, 0x7ff },
	{ G1_REG_BD_REF_PIC(4), 11, 0x7ff },
	{ G1_REG_BD_REF_PIC(4), 0, 0x7ff },
};

/* quantizer delta regs */
static const struct vp8_dec_reg vp8_dec_quant_delta[5] = {
	{ G1_REG_REF_PIC(3), 27, 0x1f },
	{ G1_REG_REF_PIC(3), 22, 0x1f },
	{ G1_REG_BD_REF_PIC(4), 27, 0x1f },
	{ G1_REG_BD_REF_PIC(4), 22, 0x1f },
	{ G1_REG_BD_P_REF_PIC, 27, 0x1f },
};

/* dct partition start bits regs */
static const struct vp8_dec_reg vp8_dec_dct_start_bits[8] = {
	{ G1_REG_DEC_CTRL2, 26, 0x3f }, { G1_REG_DEC_CTRL4, 26, 0x3f },
	{ G1_REG_DEC_CTRL4, 20, 0x3f }, { G1_REG_DEC_CTRL7, 24, 0x3f },
	{ G1_REG_DEC_CTRL7, 18, 0x3f }, { G1_REG_DEC_CTRL7, 12, 0x3f },
	{ G1_REG_DEC_CTRL7, 6, 0x3f },  { G1_REG_DEC_CTRL7, 0, 0x3f },
};

/* precision filter tap regs */
static const struct vp8_dec_reg vp8_dec_pred_bc_tap[8][4] = {
	{
		{ G1_REG_PRED_FLT, 22, 0x3ff },
		{ G1_REG_PRED_FLT, 12, 0x3ff },
		{ G1_REG_PRED_FLT, 2, 0x3ff },
		{ G1_REG_REF_PIC(4), 22, 0x3ff },
	},
	{
		{ G1_REG_REF_PIC(4), 12, 0x3ff },
		{ G1_REG_REF_PIC(4), 2, 0x3ff },
		{ G1_REG_REF_PIC(5), 22, 0x3ff },
		{ G1_REG_REF_PIC(5), 12, 0x3ff },
	},
	{
		{ G1_REG_REF_PIC(5), 2, 0x3ff },
		{ G1_REG_REF_PIC(6), 22, 0x3ff },
		{ G1_REG_REF_PIC(6), 12, 0x3ff },
		{ G1_REG_REF_PIC(6), 2, 0x3ff },
	},
	{
		{ G1_REG_REF_PIC(7), 22, 0x3ff },
		{ G1_REG_REF_PIC(7), 12, 0x3ff },
		{ G1_REG_REF_PIC(7), 2, 0x3ff },
		{ G1_REG_LT_REF, 22, 0x3ff },
	},
	{
		{ G1_REG_LT_REF, 12, 0x3ff },
		{ G1_REG_LT_REF, 2, 0x3ff },
		{ G1_REG_VALID_REF, 22, 0x3ff },
		{ G1_REG_VALID_REF, 12, 0x3ff },
	},
	{
		{ G1_REG_VALID_REF, 2, 0x3ff },
		{ G1_REG_BD_REF_PIC(0), 22, 0x3ff },
		{ G1_REG_BD_REF_PIC(0), 12, 0x3ff },
		{ G1_REG_BD_REF_PIC(0), 2, 0x3ff },
	},
	{
		{ G1_REG_BD_REF_PIC(1), 22, 0x3ff },
		{ G1_REG_BD_REF_PIC(1), 12, 0x3ff },
		{ G1_REG_BD_REF_PIC(1), 2, 0x3ff },
		{ G1_REG_BD_REF_PIC(2), 22, 0x3ff },
	},
	{
		{ G1_REG_BD_REF_PIC(2), 12, 0x3ff },
		{ G1_REG_BD_REF_PIC(2), 2, 0x3ff },
		{ G1_REG_BD_REF_PIC(3), 22, 0x3ff },
		{ G1_REG_BD_REF_PIC(3), 12, 0x3ff },
	},
};

/*
 * filter taps taken to 7-bit precision,
 * reference RFC6386#Page-16, filters[8][6]
 */
static const u32 vp8_dec_mc_filter[8][6] = {
	{ 0, 0, 128, 0, 0, 0 },
	{ 0, -6, 123, 12, -1, 0 },
	{ 2, -11, 108, 36, -8, 1 },
	{ 0, -9, 93, 50, -6, 0 },
	{ 3, -16, 77, 77, -16, 3 },
	{ 0, -6, 50, 93, -9, 0 },
	{ 1, -8, 36, 108, -11, 2 },
	{ 0, -1, 12, 123, -6, 0 }
};

static inline void vp8_dec_reg_write(struct hantro_dev *vpu,
				     const struct vp8_dec_reg *reg, u32 val)
{
	u32 v;

	v = vdpu_read(vpu, reg->base);
	v &= ~(reg->mask << reg->shift);
	v |= ((val & reg->mask) << reg->shift);
	vdpu_write_relaxed(vpu, v, reg->base);
}

/*
 * set loop filters
 */
static void cfg_lf(struct hantro_ctx *ctx,
		   const struct v4l2_ctrl_vp8_frame_header *hdr)
{
	struct hantro_dev *vpu = ctx->dev;
	const struct v4l2_vp8_segment_header *seg = &hdr->segment_header;
	const struct v4l2_vp8_loopfilter_header *lf = &hdr->lf_header;
	u32 reg;
	int i;

	if (!(seg->flags & V4L2_VP8_SEGMENT_HEADER_FLAG_ENABLED)) {
		vp8_dec_reg_write(vpu, &vp8_dec_lf_level[0], lf->level);
	} else if (seg->flags & V4L2_VP8_SEGMENT_HEADER_FLAG_DELTA_VALUE_MODE) {
		for (i = 0; i < 4; i++) {
			u32 lf_level = clamp(lf->level + seg->lf_update[i],
					     0, 63);

			vp8_dec_reg_write(vpu, &vp8_dec_lf_level[i], lf_level);
		}
	} else {
		for (i = 0; i < 4; i++)
			vp8_dec_reg_write(vpu, &vp8_dec_lf_level[i],
					  seg->lf_update[i]);
	}

	reg = G1_REG_REF_PIC_FILT_SHARPNESS(lf->sharpness_level);
	if (lf->flags & V4L2_VP8_LF_FILTER_TYPE_SIMPLE)
		reg |= G1_REG_REF_PIC_FILT_TYPE_E;
	vdpu_write_relaxed(vpu, reg, G1_REG_REF_PIC(0));

	if (lf->flags & V4L2_VP8_LF_HEADER_ADJ_ENABLE) {
		for (i = 0; i < 4; i++) {
			vp8_dec_reg_write(vpu, &vp8_dec_mb_adj[i],
					  lf->mb_mode_delta[i]);
			vp8_dec_reg_write(vpu, &vp8_dec_ref_adj[i],
					  lf->ref_frm_delta[i]);
		}
	}
}

/*
 * set quantization parameters
 */
static void cfg_qp(struct hantro_ctx *ctx,
		   const struct v4l2_ctrl_vp8_frame_header *hdr)
{
	struct hantro_dev *vpu = ctx->dev;
	const struct v4l2_vp8_segment_header *seg = &hdr->segment_header;
	const struct v4l2_vp8_quantization_header *q = &hdr->quant_header;
	int i;

	if (!(seg->flags & V4L2_VP8_SEGMENT_HEADER_FLAG_ENABLED)) {
		vp8_dec_reg_write(vpu, &vp8_dec_quant[0], q->y_ac_qi);
	} else if (seg->flags & V4L2_VP8_SEGMENT_HEADER_FLAG_DELTA_VALUE_MODE) {
		for (i = 0; i < 4; i++) {
			u32 quant = clamp(q->y_ac_qi + seg->quant_update[i],
					  0, 127);

			vp8_dec_reg_write(vpu, &vp8_dec_quant[i], quant);
		}
	} else {
		for (i = 0; i < 4; i++)
			vp8_dec_reg_write(vpu, &vp8_dec_quant[i],
					  seg->quant_update[i]);
	}

	vp8_dec_reg_write(vpu, &vp8_dec_quant_delta[0], q->y_dc_delta);
	vp8_dec_reg_write(vpu, &vp8_dec_quant_delta[1], q->y2_dc_delta);
	vp8_dec_reg_write(vpu, &vp8_dec_quant_delta[2], q->y2_ac_delta);
	vp8_dec_reg_write(vpu, &vp8_dec_quant_delta[3], q->uv_dc_delta);
	vp8_dec_reg_write(vpu, &vp8_dec_quant_delta[4], q->uv_ac_delta);
}

/*
 * set control partition and dct partition regs
 *
 * VP8 frame stream data layout:
 *
 *	                     first_part_size          parttion_sizes[0]
 *                              ^                     ^
 * src_dma                      |                     |
 * ^                   +--------+------+        +-----+-----+
 * |                   | control part  |        |           |
 * +--------+----------------+------------------+-----------+-----+-----------+
 * | tag 3B | extra 7B | hdr | mb_data | dct sz | dct part0 | ... | dct partn |
 * +--------+-----------------------------------+-----------+-----+-----------+
 *                           |         |        |                             |
 *                           v         +----+---+                             v
 *                           mb_start       |                       src_dma_end
 *                                          v
 *                                       dct size part
 *                                      (num_dct-1)*3B
 * Note:
 *   1. only key-frames have extra 7-bytes
 *   2. all offsets are base on src_dma
 *   3. number of DCT parts is 1, 2, 4 or 8
 *   4. the addresses set to the VPU must be 64-bits aligned
 */
static void cfg_parts(struct hantro_ctx *ctx,
		      const struct v4l2_ctrl_vp8_frame_header *hdr)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *vb2_src;
	u32 first_part_offset = VP8_FRAME_IS_KEY_FRAME(hdr) ? 10 : 3;
	u32 dct_part_total_len = 0;
	u32 dct_size_part_size = 0;
	u32 dct_part_offset = 0;
	u32 mb_offset_bytes = 0;
	u32 mb_offset_bits = 0;
	u32 mb_start_bits = 0;
	struct vp8_dec_reg reg;
	dma_addr_t src_dma;
	u32 mb_size = 0;
	u32 count = 0;
	u32 i;

	vb2_src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	src_dma = vb2_dma_contig_plane_dma_addr(&vb2_src->vb2_buf, 0);

	/*
	 * Calculate control partition mb data info
	 * @macroblock_bit_offset:	bits offset of mb data from first
	 *				part start pos
	 * @mb_offset_bits:		bits offset of mb data from src_dma
	 *				base addr
	 * @mb_offset_byte:		bytes offset of mb data from src_dma
	 *				base addr
	 * @mb_start_bits:		bits offset of mb data from mb data
	 *				64bits alignment addr
	 */
	mb_offset_bits = first_part_offset * 8 +
			 hdr->macroblock_bit_offset + 8;
	mb_offset_bytes = mb_offset_bits / 8;
	mb_start_bits = mb_offset_bits -
			(mb_offset_bytes & (~DEC_8190_ALIGN_MASK)) * 8;
	mb_size = hdr->first_part_size -
		  (mb_offset_bytes - first_part_offset) +
		  (mb_offset_bytes & DEC_8190_ALIGN_MASK);

	/* mb data aligned base addr */
	vdpu_write_relaxed(vpu, (mb_offset_bytes & (~DEC_8190_ALIGN_MASK))
				+ src_dma, G1_REG_ADDR_REF(13));

	/* mb data start bits */
	reg.base = G1_REG_DEC_CTRL2;
	reg.mask = 0x3f;
	reg.shift = 18;
	vp8_dec_reg_write(vpu, &reg, mb_start_bits);

	/* mb aligned data length */
	reg.base = G1_REG_DEC_CTRL6;
	reg.mask = 0x3fffff;
	reg.shift = 0;
	vp8_dec_reg_write(vpu, &reg, mb_size + 1);

	/*
	 * Calculate dct partition info
	 * @dct_size_part_size: Containing sizes of dct part, every dct part
	 *			has 3 bytes to store its size, except the last
	 *			dct part
	 * @dct_part_offset:	bytes offset of dct parts from src_dma base addr
	 * @dct_part_total_len: total size of all dct parts
	 */
	dct_size_part_size = (hdr->num_dct_parts - 1) * 3;
	dct_part_offset = first_part_offset + hdr->first_part_size;
	for (i = 0; i < hdr->num_dct_parts; i++)
		dct_part_total_len += hdr->dct_part_sizes[i];
	dct_part_total_len += dct_size_part_size;
	dct_part_total_len += (dct_part_offset & DEC_8190_ALIGN_MASK);

	/* number of dct partitions */
	reg.base = G1_REG_DEC_CTRL6;
	reg.mask = 0xf;
	reg.shift = 24;
	vp8_dec_reg_write(vpu, &reg, hdr->num_dct_parts - 1);

	/* dct partition length */
	vdpu_write_relaxed(vpu,
			   G1_REG_DEC_CTRL3_STREAM_LEN(dct_part_total_len),
			   G1_REG_DEC_CTRL3);

	/* dct partitions base address */
	for (i = 0; i < hdr->num_dct_parts; i++) {
		u32 byte_offset = dct_part_offset + dct_size_part_size + count;
		u32 base_addr = byte_offset + src_dma;

		vp8_dec_reg_write(vpu, &vp8_dec_dct_base[i],
				  base_addr & (~DEC_8190_ALIGN_MASK));

		vp8_dec_reg_write(vpu, &vp8_dec_dct_start_bits[i],
				  (byte_offset & DEC_8190_ALIGN_MASK) * 8);

		count += hdr->dct_part_sizes[i];
	}
}

/*
 * prediction filter taps
 * normal 6-tap filters
 */
static void cfg_tap(struct hantro_ctx *ctx,
		    const struct v4l2_ctrl_vp8_frame_header *hdr)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vp8_dec_reg reg;
	u32 val = 0;
	int i, j;

	reg.base = G1_REG_BD_REF_PIC(3);
	reg.mask = 0xf;

	if ((hdr->version & 0x03) != 0)
		return; /* Tap filter not used. */

	for (i = 0; i < 8; i++) {
		val = (vp8_dec_mc_filter[i][0] << 2) | vp8_dec_mc_filter[i][5];

		for (j = 0; j < 4; j++)
			vp8_dec_reg_write(vpu, &vp8_dec_pred_bc_tap[i][j],
					  vp8_dec_mc_filter[i][j + 1]);

		switch (i) {
		case 2:
			reg.shift = 8;
			break;
		case 4:
			reg.shift = 4;
			break;
		case 6:
			reg.shift = 0;
			break;
		default:
			continue;
		}

		vp8_dec_reg_write(vpu, &reg, val);
	}
}

/* set reference frame */
static void cfg_ref(struct hantro_ctx *ctx,
		    const struct v4l2_ctrl_vp8_frame_header *hdr)
{
	struct vb2_queue *cap_q = &ctx->fh.m2m_ctx->cap_q_ctx.q;
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *vb2_dst;
	u32 reg;

	vb2_dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* set last frame address */
	reg = hantro_get_ref(cap_q, hdr->last_frame_ts);
	if (!reg)
		reg = vb2_dma_contig_plane_dma_addr(&vb2_dst->vb2_buf, 0);
	vdpu_write_relaxed(vpu, reg, G1_REG_ADDR_REF(0));

	/* set golden reference frame buffer address */
	reg = hantro_get_ref(cap_q, hdr->golden_frame_ts);
	WARN_ON(!reg && hdr->golden_frame_ts);
	if (!reg)
		reg = vb2_dma_contig_plane_dma_addr(&vb2_dst->vb2_buf, 0);
	if (hdr->flags & V4L2_VP8_FRAME_HEADER_FLAG_SIGN_BIAS_GOLDEN)
		reg |= G1_REG_ADDR_REF_TOPC_E;
	vdpu_write_relaxed(vpu, reg, G1_REG_ADDR_REF(4));

	/* set alternate reference frame buffer address */
	reg = hantro_get_ref(cap_q, hdr->alt_frame_ts);
	WARN_ON(!reg && hdr->alt_frame_ts);
	if (!reg)
		reg = vb2_dma_contig_plane_dma_addr(&vb2_dst->vb2_buf, 0);
	if (hdr->flags & V4L2_VP8_FRAME_HEADER_FLAG_SIGN_BIAS_ALT)
		reg |= G1_REG_ADDR_REF_TOPC_E;
	vdpu_write_relaxed(vpu, reg, G1_REG_ADDR_REF(5));
}

static void cfg_buffers(struct hantro_ctx *ctx,
			const struct v4l2_ctrl_vp8_frame_header *hdr)
{
	const struct v4l2_vp8_segment_header *seg = &hdr->segment_header;
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *vb2_dst;
	dma_addr_t dst_dma;
	u32 reg;

	vb2_dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* set probability table buffer address */
	vdpu_write_relaxed(vpu, ctx->vp8_dec.prob_tbl.dma,
			   G1_REG_ADDR_QTABLE);

	/* set segment map address */
	reg = G1_REG_FWD_PIC1_SEGMENT_BASE(ctx->vp8_dec.segment_map.dma);
	if (seg->flags & V4L2_VP8_SEGMENT_HEADER_FLAG_ENABLED) {
		reg |= G1_REG_FWD_PIC1_SEGMENT_E;
		if (seg->flags & V4L2_VP8_SEGMENT_HEADER_FLAG_UPDATE_MAP)
			reg |= G1_REG_FWD_PIC1_SEGMENT_UPD_E;
	}
	vdpu_write_relaxed(vpu, reg, G1_REG_FWD_PIC(0));

	/* set output frame buffer address */
	dst_dma = vb2_dma_contig_plane_dma_addr(&vb2_dst->vb2_buf, 0);
	vdpu_write_relaxed(vpu, dst_dma, G1_REG_ADDR_DST);
}

void hantro_g1_vp8_dec_run(struct hantro_ctx *ctx)
{
	const struct v4l2_ctrl_vp8_frame_header *hdr;
	struct hantro_dev *vpu = ctx->dev;
	size_t height = ctx->dst_fmt.height;
	size_t width = ctx->dst_fmt.width;
	struct vb2_v4l2_buffer *vb2_src;
	u32 mb_width, mb_height;
	u32 reg;

	vb2_src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	v4l2_ctrl_request_setup(vb2_src->vb2_buf.req_obj.req,
				&ctx->ctrl_handler);

	hdr = hantro_get_ctrl(ctx, V4L2_CID_MPEG_VIDEO_VP8_FRAME_HEADER);
	if (WARN_ON(!hdr))
		return;

	/* reset segment_map buffer in keyframe */
	if (VP8_FRAME_IS_KEY_FRAME(hdr) && ctx->vp8_dec.segment_map.cpu)
		memset(ctx->vp8_dec.segment_map.cpu, 0,
		       ctx->vp8_dec.segment_map.size);

	hantro_vp8_prob_update(ctx, hdr);

	reg = G1_REG_CONFIG_DEC_TIMEOUT_E |
	      G1_REG_CONFIG_DEC_STRENDIAN_E |
	      G1_REG_CONFIG_DEC_INSWAP32_E |
	      G1_REG_CONFIG_DEC_STRSWAP32_E |
	      G1_REG_CONFIG_DEC_OUTSWAP32_E |
	      G1_REG_CONFIG_DEC_CLK_GATE_E |
	      G1_REG_CONFIG_DEC_IN_ENDIAN |
	      G1_REG_CONFIG_DEC_OUT_ENDIAN |
	      G1_REG_CONFIG_DEC_MAX_BURST(16);
	vdpu_write_relaxed(vpu, reg, G1_REG_CONFIG);

	reg = G1_REG_DEC_CTRL0_DEC_MODE(10);
	if (!VP8_FRAME_IS_KEY_FRAME(hdr))
		reg |= G1_REG_DEC_CTRL0_PIC_INTER_E;
	if (!(hdr->flags & V4L2_VP8_FRAME_HEADER_FLAG_MB_NO_SKIP_COEFF))
		reg |= G1_REG_DEC_CTRL0_SKIP_MODE;
	if (hdr->lf_header.level == 0)
		reg |= G1_REG_DEC_CTRL0_FILTERING_DIS;
	vdpu_write_relaxed(vpu, reg, G1_REG_DEC_CTRL0);

	/* frame dimensions */
	mb_width = DIV_ROUND_UP(width, 16);
	mb_height = DIV_ROUND_UP(height, 16);
	reg = G1_REG_DEC_CTRL1_PIC_MB_WIDTH(mb_width) |
	      G1_REG_DEC_CTRL1_PIC_MB_HEIGHT_P(mb_height) |
	      G1_REG_DEC_CTRL1_PIC_MB_W_EXT(mb_width >> 9) |
	      G1_REG_DEC_CTRL1_PIC_MB_H_EXT(mb_height >> 8);
	vdpu_write_relaxed(vpu, reg, G1_REG_DEC_CTRL1);

	/* bool decode info */
	reg = G1_REG_DEC_CTRL2_BOOLEAN_RANGE(hdr->coder_state.range)
		| G1_REG_DEC_CTRL2_BOOLEAN_VALUE(hdr->coder_state.value);
	vdpu_write_relaxed(vpu, reg, G1_REG_DEC_CTRL2);

	reg = 0;
	if (hdr->version != 3)
		reg |= G1_REG_DEC_CTRL4_VC1_HEIGHT_EXT;
	if (hdr->version & 0x3)
		reg |= G1_REG_DEC_CTRL4_BILIN_MC_E;
	vdpu_write_relaxed(vpu, reg, G1_REG_DEC_CTRL4);

	cfg_lf(ctx, hdr);
	cfg_qp(ctx, hdr);
	cfg_parts(ctx, hdr);
	cfg_tap(ctx, hdr);
	cfg_ref(ctx, hdr);
	cfg_buffers(ctx, hdr);

	/* Controls no longer in-use, we can complete them */
	v4l2_ctrl_request_complete(vb2_src->vb2_buf.req_obj.req,
				   &ctx->ctrl_handler);

	schedule_delayed_work(&vpu->watchdog_work, msecs_to_jiffies(2000));

	vdpu_write(vpu, G1_REG_INTERRUPT_DEC_E, G1_REG_INTERRUPT);
}
