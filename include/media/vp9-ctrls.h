/* SPDX-License-Identifier: GPL-2.0 */
/*
 * These are the VP9 state controls for use with stateless VP9
 * codec drivers.
 *
 * It turns out that these structs are not stable yet and will undergo
 * more changes. So keep them private until they are stable and ready to
 * become part of the official public API.
 */

#ifndef _VP9_CTRLS_H_
#define _VP9_CTRLS_H_

#include <linux/types.h>

#define V4L2_PIX_FMT_VP9_FRAME v4l2_fourcc('V', 'P', '9', 'F')

#define V4L2_CID_MPEG_VIDEO_VP9_FRAME_HEADER	(V4L2_CID_MPEG_BASE + 4000)
#define V4L2_CTRL_TYPE_VP9_FRAME_DECODE_PARAMS	0x400
#define V4L2_CTRL_TYPE_VP9_FRAME_CONTEXT	0x401

#define V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED		(1 << 0)
#define V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE		(1 << 1)

struct v4l2_vp9_loop_filter {
	__u8 flags;
	__u8 level;
	__u8 sharpness;
	__s8 ref_deltas[4];
	__s8 mode_deltas[2];
	__u8 lvl_lookup[8][4][2];
};

#define V4L2_VP9_QUANTIZATION_FLAG_LOSSLESS		(1 << 0)

struct v4l2_vp9_quantization {
	__u8 flags;
	__u8 base_q_idx;
	__s8 delta_q_y_dc;
	__s8 delta_q_uv_dc;
	__s8 delta_q_uv_ac;
	__u8 padding[3];
};

#define V4L2_VP9_SEGMENTATION_FLAG_ENABLED		(1 << 0)
#define V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP		(1 << 1)
#define V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE	(1 << 2)
#define V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA		(1 << 3)
#define V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE	(1 << 4)

struct v4l2_vp9_segmentation {
	__u8 flags;
	__u8 tree_probs[7];
	__u8 pred_probs[3];
	__u8 padding[5];
	__u8 feature_enabled[8][4];
	__s16 feature_data[8][4];
};

struct v4l2_vp9_probs {
	__u8 tx_probs_8x8[2][1];
	__u8 tx_probs_16x16[2][2];
	__u8 tx_probs_32x32[2][3];

	__u8 coef_probs[4][2][2][6][6][3];
	__u8 skip_prob[3];
	__u8 inter_mode_probs[7][3];
	__u8 interp_filter_probs[4][2];
	__u8 is_inter_prob[4];

	__u8 comp_mode_prob[5];
	__u8 single_ref_prob[5][2];
	__u8 comp_ref_prob[5];

	__u8 y_mode_probs[4][9];
	__u8 uv_mode_probs[10][9];

	__u8 partition_probs[16][3];

	__u8 mv_joint_probs[3];
	__u8 mv_sign_prob[2];
	__u8 mv_class_probs[2][10];
	__u8 mv_class0_bit_prob[2];
	__u8 mv_bits_prob[2][10];
	__u8 mv_class0_fr_probs[2][2][3];
	__u8 mv_fr_probs[2][3];
	__u8 mv_class0_hp_prob[2];
	__u8 mv_hp_prob[2];
};

#define V4L2_VP9_FRAME_FLAG_KEY_FRAME		(1 << 0)
#define V4L2_VP9_FRAME_FLAG_SHOW_FRAME		(1 << 1)
#define V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT	(1 << 2)
#define V4L2_VP9_FRAME_FLAG_INTRA_ONLY		(1 << 3)
#define V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV	(1 << 4)
#define V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX	(1 << 5)
#define V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE	(1 << 6)
#define V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING	(1 << 7)
#define V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING	(1 << 8)
#define V4L2_VP9_FRAME_COLOR_RANGE_FULL_SWING	(1 << 9)

#define VP9_PROFILE_MAX		3

/**
 * enum v4l2_vp9_reset_frame_context - Valid values for
 *			&v4l2_ctrl_vp9_frame_decode_params->reset_frame_context
 *
 * @V4L2_VP9_RESET_FRAME_CTX_NONE: don't reset any frame context
 * @V4L2_VP9_RESET_FRAME_CTX_NONE_ALT: don't reset any frame context. This is an
 *				       alternate value for
 *				       V4L2_VP9_RESET_FRAME_CTX_NONE but has
 *				       the same meaning
 * @V4L2_VP9_RESET_FRAME_CTX_SPEC: reset the frame context pointed by
 *				   &v4l2_ctrl_vp9_frame_decode_params->frame_context_idx.
 * @V4L2_VP9_RESET_FRAME_CTX_ALL: reset all frame contexts
 */
enum v4l2_vp9_reset_frame_context {
	V4L2_VP9_RESET_FRAME_CTX_NONE,
	V4L2_VP9_RESET_FRAME_CTX_NONE_ALT,
	V4L2_VP9_RESET_FRAME_CTX_SPEC,
	V4L2_VP9_RESET_FRAME_CTX_ALL,
}

/**
 * enum v4l2_vp9_color_space - Valid values for
 *			&v4l2_ctrl_vp9_frame_decode_params->color_space
 *
 * @V4L2_VP9_COLOR_SPACE_UNKNOWN: unknown color space. In this case the color
 *				  space must be signaled outside the VP9
 *				  bitstream
 * @V4L2_VP9_COLOR_SPACE_BT_601: Rec. ITU-R BT.601-7
 * @V4L2_VP9_COLOR_SPACE_BT_709: Rec. ITU-R BT.709-6
 * @V4L2_VP9_COLOR_SPACE_SMPTE_170: SMPTE-170
 * @V4L2_VP9_COLOR_SPACE_SMPTE_240: SMPTE-240
 * @V4L2_VP9_COLOR_SPACE_BT_2020: Rec. ITU-R BT.2020-2
 * @V4L2_VP9_COLOR_SPACE_RESERVED: reserved. This value should never be passed
 * @V4L2_VP9_COLOR_SPACE_SRGB: sRGB (IEC 61966-2-1)
 */
enum v4l2_vp9_color_space {
	V4L2_VP9_COLOR_SPACE_UNKNOWN,
	V4L2_VP9_COLOR_SPACE_BT_601,
	V4L2_VP9_COLOR_SPACE_BT_709,
	V4L2_VP9_COLOR_SPACE_SMPTE_170,
	V4L2_VP9_COLOR_SPACE_SMPTE_240,
	V4L2_VP9_COLOR_SPACE_BT_2020,
	V4L2_VP9_COLOR_SPACE_RESERVED,
	V4L2_VP9_COLOR_SPACE_SRGB,
};

/**
 * enum v4l2_vp9_color_space - Valid values for
 *		&v4l2_ctrl_vp9_frame_decode_params->interpolation_filter
 *
 * @V4L2_VP9_INTERP_FILTER_8TAP: height tap filter
 * @V4L2_VP9_INTERP_FILTER_8TAP_SMOOTH: height tap smooth filter
 * @V4L2_VP9_INTERP_FILTER_8TAP_SHARP: height tap sharp filter
 * @V4L2_VP9_INTERP_FILTER_BILINEAR: bilinear filter
 * @V4L2_VP9_INTERP_FILTER_SWITCHABLE: filter selection is signaled at the
 *				       block level
 */
enum v4l2_vp9_interpolation_filter {
	V4L2_VP9_INTERP_FILTER_8TAP,
	V4L2_VP9_INTERP_FILTER_8TAP_SMOOTH,
	V4L2_VP9_INTERP_FILTER_8TAP_SHARP,
	V4L2_VP9_INTERP_FILTER_BILINEAR,
	V4L2_VP9_INTERP_FILTER_SWITCHABLE,
};

/**
 * enum v4l2_vp9_reference_mode - Valid values for
 *			&v4l2_ctrl_vp9_frame_decode_params->reference_mode
 *
 * @V4L2_VP9_REF_MODE_SINGLE: indicates that all the inter blocks use only a
 *			      single reference frame to generate motion
 *			      compensated prediction
 * @V4L2_VP9_REF_MODE_COMPOUND: requires all the inter blocks to use compound
 *				mode. Single reference frame prediction is not
 *				allowed
 * @V4L2_VP9_REF_MODE_SELECT: allows each individual inter block to select
 *			      between single and compound prediction modes
 */
enum v4l2_vp9_reference_mode {
	V4L2_VP9_REF_MODE_SINGLE,
	V4L2_VP9_REF_MODE_COMPOUND,
	V4L2_VP9_REF_MODE_SELECT,
};

/**
 * enum v4l2_vp9_tx_mode - Valid values for
 *		&v4l2_ctrl_vp9_frame_decode_params->tx_mode
 *
 * @V4L2_VP9_TX_MODE_ONLY_4X4: transform size is 4x4
 * @V4L2_VP9_TX_MODE_ALLOW_8X8: transform size can be up to 8x8
 * @V4L2_VP9_TX_MODE_ALLOW_16X16: transform size can be up to 16x16
 * @V4L2_VP9_TX_MODE_ALLOW_32X32: transform size can be up to 32x32
 * @V4L2_VP9_TX_MODE_SELECT: bitstream contains transform size for each block
 */
enum v4l2_vp9_tx_mode {
	V4L2_VP9_TX_MODE_ONLY_4X4,
	V4L2_VP9_TX_MODE_ALLOW_8X8,
	V4L2_VP9_TX_MODE_ALLOW_16X16,
	V4L2_VP9_TX_MODE_ALLOW_32X32,
	V4L2_VP9_TX_MODE_SELECT,
};

/**
 * struct v4l2_vp9_reference_frame - VP9 reference frame info
 *
 * @timestamp: reference buffer timestamp
 * @flags: only X/Y_SUBSAMPLING are meaningful here
 * @width: frame width
 * @height: frame height
 * @bit_depth: Y/UV component depth. Can be 8, 10 or 12
 * @padding: must be 0
 */
struct v4l2_vp9_reference_frame {
	__u64 timestamp;
	__u32 flags;
	__u16 width;
	__u16 height;
	__u8 bit_depth;
	__u8 padding[7];
};

/**
 * struct v4l2_ctrl_vp9_frame_decode_params - VP9 frame decoding control
 *
 * @flags: combination of V4L2_VP9_FRAME_FLAG_* flags
 * @header_size_in_bytes: indicates the size of the compressed header in bytes
 * @profile: VP9 profile. Can be 0, 1, 2 or 3
 * @reset_frame_context: specifies whether the frame context should be reset
 *			 to default values. See &v4l2_vp9_reset_frame_context
 *			 for more details
 * @bit_depth: bits per components. Can be 8, 10 or 12. Note that not all
 *	       profiles support 10 and/or 12 bits depths
 * @color_space: specifies the color space of the stream. See
 *		 &v4l2_vp9_color_space for more details
 * @interpolation_filter: specifies the filter selection used for performing
 *			  inter prediction. See &v4l2_vp9_interpolation_filter
 *			  for more details
 * @tile_cols_log2: specifies the base 2 logarithm of the width of each tile
 *		    (where the width is measured in units of 8x8 blocks).
 *		    Shall be less than or equal to 6
 * @tile_rows_log2: specifies the base 2 logarithm of the height of each tile
 *		    (where the height is measured in units of 8x8 blocks)
 * @reference_mode: specifies the type of inter prediction to be used. See
 *		    &v4l2_vp9_reference_mode for more details
 * @frame_witdh_minus_1: add 1 to it and you'll get the frame width expressed
 *			 in pixels
 * @frame_height_minus_1: add 1 to it and you'll get the frame height expressed
 *			  in pixels
 * @frame_witdh_minus_1: add 1 to it and you'll get the expected render width
 *			 expressed in pixels. This is not used during the
 *			 decoding process but might be used by HW scalers to
 *			 prepare a frame that's ready for scanout
 * @frame_height_minus_1: add 1 to it and you'll get the expected render height
 *			 expressed in pixels. This is not used during the
 *			 decoding process but might be used by HW scalers to
 *			 prepare a frame that's ready for scanout
 * @last_frame: reference to the last frame
 * @golden_frame: reference to the golden frame
 * @alt_frame: reference to the alt frame
 * @lf: loop filter parameters. See &v4l2_vp9_loop_filter for more details
 * @quant: quantization parameters. See &v4l2_vp9_quantization for more details
 * @seg: segmentation parameters. See &v4l2_vp9_segmentation for more details
 */
struct v4l2_ctrl_vp9_frame_decode_params {
	__u32 flags;
	__u16 header_size_in_bytes;
	__u8 profile;
	__u8 reset_frame_context;
	__u8 frame_context_idx;
	__u8 bit_depth;
	__u8 color_space;
	__u8 interpolation_filter;
	__u8 tile_cols_log2;
	__u8 tile_rows_log2;
	__u8 tx_mode;
	__u8 reference_mode;
	__u16 frame_witdh_minus_1;
	__u16 frame_height_minus_1;
	__u16 render_witdh_minus_1;
	__u16 render_height_minus_1;
	struct v4l2_vp9_reference_frame last_frame;
	struct v4l2_vp9_reference_frame golden_frame;
	struct v4l2_vp9_reference_frame alt_frame;
	struct v4l2_vp9_loop_filter lf;
	struct v4l2_vp9_quantization quant;
	struct v4l2_vp9_segmentation seg;
	struct v4l2_vp9_probs probs; 
};

#define V4L2_CTRL_VP9_NUM_FRAME_CTX	4

/**
 * struct v4l2_ctrl_vp9_frame_ctx - VP9 frame context control
 *
 * @probs: probabilities
 *
 * This control is accessed in both direction. The user should initialize the
 * 4 contexts with default values just after starting the stream (can it be
 * automated in kernel space?). Then before decoding a frame it should query
 * the current frame context (the one passed through
 * &v4l2_ctrl_vp9_frame_decode_params->frame_context_idx) to initialize
 * &v4l2_ctrl_vp9_frame_decode_params->probs. The probs are then adjusted based
 * on the bitstream info and passed to the kernel. The codec should update
 * the frame context after the frame has been decoded, so that next time
 * userspace query this context it contains the updated probs.
 */
struct v4l2_ctrl_vp9_frame_ctx {
	struct v4l2_vp9_probs probs;
};

#endif /* _VP9_CTRLS_H_ */
