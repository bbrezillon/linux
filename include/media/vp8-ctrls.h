/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TODO: Make sure structs have no holes
 * and are 4-byte aligned.
 */

#ifndef _VP8_CTRLS_H_
#define _VP8_CTRLS_H_

#include <linux/v4l2-controls.h>

#define V4L2_CID_MPEG_VIDEO_VP8_FRAME_HDR (V4L2_CID_MPEG_BASE + 590)
#define V4L2_CTRL_TYPE_VP8_FRAME_HDR 0x301

#define V4L2_VP8_SEGMNT_HDR_FLAG_ENABLED              0x01
#define V4L2_VP8_SEGMNT_HDR_FLAG_UPDATE_MAP           0x02
#define V4L2_VP8_SEGMNT_HDR_FLAG_UPDATE_FEATURE_DATA  0x04

struct v4l2_vp8_segment_header {
	__u8 segment_feature_mode;
	__s8 quant_update[4];
	__s8 lf_update[4];
	__u8 segment_probs[3];
	__u8 flags;
};

#define V4L2_VP8_LF_HDR_ADJ_ENABLE	0x01
#define V4L2_VP8_LF_HDR_DELTA_UPDATE	0x02
struct v4l2_vp8_loopfilter_header {
	__u8 type;
	__u8 level;
	__u8 sharpness_level;
	__s8 ref_frm_delta_magnitude[4];
	__s8 mb_mode_delta_magnitude[4];
	__u8 flags;
};

struct v4l2_vp8_quantization_header {
	__u8 y_ac_qi;
	__s8 y_dc_delta;
	__s8 y2_dc_delta;
	__s8 y2_ac_delta;
	__s8 uv_dc_delta;
	__s8 uv_ac_delta;
	__u16 dequant_factors[4][3][2];
};

struct v4l2_vp8_entropy_header {
	__u8 coeff_probs[4][8][3][11];
	__u8 y_mode_probs[4];
	__u8 uv_mode_probs[3];
	__u8 mv_probs[2][19];
};

#define V4L2_VP8_FRAME_HDR_FLAG_EXPERIMENTAL		0x01
#define V4L2_VP8_FRAME_HDR_FLAG_SHOW_FRAME		0x02
#define V4L2_VP8_FRAME_HDR_FLAG_MB_NO_SKIP_COEFF	0x04
struct v4l2_ctrl_vp8_frame_header {
	/* 0: keyframe, 1: not a keyframe */
	__u8 key_frame;
	__u8 version;

	/* Populated also if not a key frame */
	__u16 width;
	__u8 horizontal_scale;
	__u16 height;
	__u8 vertical_scale;

	struct v4l2_vp8_segment_header segment_header;
	struct v4l2_vp8_loopfilter_header lf_header;
	struct v4l2_vp8_quantization_header quant_header;
	struct v4l2_vp8_entropy_header entropy_header;

	__u8 sign_bias_golden;
	__u8 sign_bias_alternate;

	__u8 prob_skip_false;
	__u8 prob_intra;
	__u8 prob_last;
	__u8 prob_gf;

	__u32 first_part_size;
	__u32 first_part_offset;
	/*
	 * Offset in bits of MB data in first partition,
	 * i.e. bit offset starting from first_part_offset.
	 */
	__u32 macroblock_bit_offset;

	__u32 dct_part_sizes[8];
	__u8 num_dct_parts;

	__u8 bool_dec_range;
	__u8 bool_dec_value;
	__u8 bool_dec_count;

	/* v4l2_buffer timestamps of reference frames */
	__u64 last_frame_ts;
	__u64 golden_frame_ts;
	__u64 alt_frame_ts;

	__u8 flags;
};

#endif
