// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder VP9 backend
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *	Boris Brezillon <boris.brezillon@collabora.com>
 *
 * Copyright (C) 2016 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 */

#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <media/v4l2-mem2mem.h>

#include "rkvdec.h"
#include "rkvdec-regs.h"

#define RKVDEC_VP9_PROBE_SIZE		4864
#define RKVDEC_VP9_COUNT_SIZE		13232
#define RKVDEC_VP9_MAX_SEGMAP_SIZE	73728

struct rkvdec_vp9_intra_mode_probs {
	u8 y_mode_prob[105];
	u8 uv_mode_prob[23];

};

struct rkvdec_vp9_intra_only_frame_probs {
	u8 coef_probs_intra[4][2][128];
	struct rkvdec_vp9_intra_mode_probs intra_mode[10];
};

struct rkvdec_vp9_inter_frame_probs {
	u8 y_mode_probs[4][9];
	u8 comp_mode_prob[5];
	u8 comp_ref_prob[5];
	u8 single_ref_prob[5][2];
	u8 inter_mode_probs[7][3];
	u8 interp_filter_probs[4][2];
	u8 padding0[11];
	u8 coef_probs[2][4][2][128];
	u8 uv_mode_prob_0_2[3][9];
	u8 padding1[5];
	u8 uv_mode_prob_3_5[3][9];
	u8 padding2[5];
	u8 uv_mode_prob_6_8[3][9];
	u8 padding3[5];
	u8 uv_mode_prob_9[9];
	u8 padding4[7];
	u8 padding5[16];
	u8 mv_joint_probs[3];
	u8 mv_sign_prob[2];
	u8 mv_class_probs[2][10];
	u8 mv_class0_bit_prob[2];
	u8 mv_bits_prob[2][10];
	u8 mv_class0_fr_probs[2][2][3];
	u8 mv_fr_probs[2][3];
	u8 mv_class0_hp_prob[2];
	u8 mv_hp_prob[2];
};

struct rkvdec_vp9_probs {
	u8 partition_probs[16][3];
	u8 pred_probs[3];
	u8 tree_probs[7];
	u8 skip_prob[3];
	u8 tx_probs_32x32[2][3];
	u8 tx_probs_16x16[2][2];
	u8 tx_probs_8x8[2][1];
	u8 is_inter_prob[4];
	/* 128 bit alignment */
	u8 padding0[3];
	union {
		struct rkvdec_vp9_inter_frame_probs inter_probs;
		struct rkvdec_vp9_intra_only_frame_probs intra_only_probs;
	};

	/* Is this really needed? */
//	u8 padding1[2416];
};

/* Data structure describing auxiliary buffer format. */
struct rkvdec_vp9_priv_tbl {
	union {
		struct rkvdec_vp9_probs probs;
//		u8 raw[4864];
	};
	u8 segmap[2][RKVDEC_VP9_MAX_SEGMAP_SIZE];
};

struct rkvdec_vp9_refs_counts {
	u32 eob[2];
	u32 coeff[3];
};

struct rkvdec_vp9_inter_frame_symbol_counts {
	u32 partition[16][4];
	u32 skip[3][2];
	u32 inter[4][2];
	u32 tx32p[2][4];
	u32 tx16p[2][4];
	u32 tx8p[2][2];
	u32 y_mode[4][10];
	u32 uv_mode[10][10];
	u32 comp[5][2];
	u32 comp_ref[5][2];
	u32 single_ref[5][2][2];
	u32 mv_mode[7][4];
	u32 filter[4][3];
	u32 mv_joint[4];
	u32 sign[2][2];
	/* add 1 element for align */
	u32 classes[2][11 + 1];
	u32 class0[2][2];
	u32 bits[2][10][2];
	u32 class0_fp[2][2][4];
	u32 fp[2][4];
	u32 class0_hp[2][2];
	u32 hp[2][2];
	struct rkvdec_vp9_refs_counts ref_cnt[2][4][2][6][6];
};

struct rkvdec_vp9_intra_frame_symbol_counts {
	u32 partition[4][4][4];
	u32 skip[3][2];
	u32 intra[4][2];
	u32 tx32p[2][4];
	u32 tx16p[2][4];
	u32 tx8p[2][2];
	struct rkvdec_vp9_refs_counts ref_cnt[2][4][2][6][6];
};

struct rkvdec_vp9_run {
	struct rkvdec_run base;
	const struct v4l2_ctrl_vp9_frame_decode_params *decode_params;
};

struct rkvdec_vp9_ctx {
	struct rkvdec_aux_buf priv_tbl;
	struct rkvdec_aux_buf count_tbl;
};

#ifndef FASTDIV
#define FASTDIV(a, b) ((u32)((((u64)a) * inverse[b]) >> 32))
#endif /* FASTDIV */

static const u32 inverse[] = {
	0, 4294967295U, 2147483648U, 1431655766, 1073741824,  858993460,
	715827883,  613566757,	536870912,  477218589,  429496730,  390451573,
	357913942,  330382100,  306783379,  286331154,	268435456,  252645136,
	238609295,  226050911,  214748365,  204522253,  195225787,  186737709,
	178956971,  171798692,  165191050,  159072863,  153391690,  148102321,
	143165577,  138547333,	134217728,  130150525,  126322568,  122713352,
	119304648,  116080198,  113025456,  110127367,	107374183,  104755300,
	102261127,   99882961,   97612894,   95443718,   93368855,   91382283,
	89478486,   87652394,   85899346,   84215046,   82595525,   81037119,
	79536432,   78090315,	76695845,   75350304,   74051161,   72796056,
	71582789,   70409300,   69273667,   68174085,	67108864,   66076420,
	65075263,   64103990,   63161284,   62245903,   61356676,   60492498,
	59652324,   58835169,   58040099,   57266231,   56512728,   55778797,
	55063684,   54366675,	53687092,   53024288,   52377650,   51746594,
	51130564,   50529028,   49941481,   49367441,	48806447,   48258060,
	47721859,   47197443,   46684428,   46182445,   45691142,   45210183,
	44739243,   44278014,   43826197,   43383509,   42949673,   42524429,
	42107523,   41698712,	41297763,   40904451,   40518560,   40139882,
	39768216,   39403370,   39045158,   38693400,	38347923,   38008561,
	37675152,   37347542,   37025581,   36709123,   36398028,   36092163,
	35791395,   35495598,   35204650,   34918434,   34636834,   34359739,
	34087043,   33818641,	33554432,   33294321,   33038210,   32786010,
	32537632,   32292988,   32051995,   31814573,	31580642,   31350127,
	31122952,   30899046,   30678338,   30460761,   30246249,   30034737,
	29826162,   29620465,   29417585,   29217465,   29020050,   28825284,
	28633116,   28443493,	28256364,   28071682,   27889399,   27709467,
	27531842,   27356480,   27183338,   27012373,	26843546,   26676816,
	26512144,   26349493,   26188825,   26030105,   25873297,   25718368,
	25565282,   25414008,   25264514,   25116768,   24970741,   24826401,
	24683721,   24542671,	24403224,   24265352,   24129030,   23994231,
	23860930,   23729102,   23598722,   23469767,	23342214,   23216040,
	23091223,   22967740,   22845571,   22724695,   22605092,   22486740,
	22369622,   22253717,   22139007,   22025474,   21913099,   21801865,
	21691755,   21582751,	21474837,   21367997,   21262215,   21157475,
	21053762,   20951060,   20849356,   20748635,	20648882,   20550083,
	20452226,   20355296,   20259280,   20164166,   20069941,   19976593,
	19884108,   19792477,   19701685,   19611723,   19522579,   19434242,
	19346700,   19259944,	19173962,   19088744,   19004281,   18920561,
	18837576,   18755316,   18673771,   18592933,	18512791,   18433337,
	18354562,   18276457,   18199014,   18122225,   18046082,   17970575,
	17895698,   17821442,   17747799,   17674763,   17602325,   17530479,
	17459217,   17388532,	17318417,   17248865,   17179870,   17111424,
	17043522,   16976156,   16909321,   16843010,   16777216
};

static u8 vp9_kf_y_mode_prob[10][10][9] = {
	{
		/* above = dc */
		{ 137,  30,  42, 148, 151, 207,  70,  52,  91 },/*left = dc  */
		{  92,  45, 102, 136, 116, 180,  74,  90, 100 },/*left = v   */
		{  73,  32,  19, 187, 222, 215,  46,  34, 100 },/*left = h   */
		{  91,  30,  32, 116, 121, 186,  93,  86,  94 },/*left = d45 */
		{  72,  35,  36, 149,  68, 206,  68,  63, 105 },/*left = d135*/
		{  73,  31,  28, 138,  57, 124,  55, 122, 151 },/*left = d117*/
		{  67,  23,  21, 140, 126, 197,  40,  37, 171 },/*left = d153*/
		{  86,  27,  28, 128, 154, 212,  45,  43,  53 },/*left = d207*/
		{  74,  32,  27, 107,  86, 160,  63, 134, 102 },/*left = d63 */
		{  59,  67,  44, 140, 161, 202,  78,  67, 119 } /*left = tm  */
	}, {  /* above = v */
		{  63,  36, 126, 146, 123, 158,  60,  90,  96 },/*left = dc  */
		{  43,  46, 168, 134, 107, 128,  69, 142,  92 },/*left = v   */
		{  44,  29,  68, 159, 201, 177,  50,  57,  77 },/*left = h   */
		{  58,  38,  76, 114,  97, 172,  78, 133,  92 },/*left = d45 */
		{  46,  41,  76, 140,  63, 184,  69, 112,  57 },/*left = d135*/
		{  38,  32,  85, 140,  46, 112,  54, 151, 133 },/*left = d117*/
		{  39,  27,  61, 131, 110, 175,  44,  75, 136 },/*left = d153*/
		{  52,  30,  74, 113, 130, 175,  51,  64,  58 },/*left = d207*/
		{  47,  35,  80, 100,  74, 143,  64, 163,  74 },/*left = d63 */
		{  36,  61, 116, 114, 128, 162,  80, 125,  82 } /*left = tm  */
	}, {  /* above = h */
		{  82,  26,  26, 171, 208, 204,  44,  32, 105 },/*left = dc  */
		{  55,  44,  68, 166, 179, 192,  57,  57, 108 },/*left = v   */
		{  42,  26,  11, 199, 241, 228,  23,  15,  85 },/*left = h   */
		{  68,  42,  19, 131, 160, 199,  55,  52,  83 },/*left = d45 */
		{  58,  50,  25, 139, 115, 232,  39,  52, 118 },/*left = d135*/
		{  50,  35,  33, 153, 104, 162,  64,  59, 131 },/*left = d117*/
		{  44,  24,  16, 150, 177, 202,  33,  19, 156 },/*left = d153*/
		{  55,  27,  12, 153, 203, 218,  26,  27,  49 },/*left = d207*/
		{  53,  49,  21, 110, 116, 168,  59,  80,  76 },/*left = d63 */
		{  38,  72,  19, 168, 203, 212,  50,  50, 107 } /*left = tm  */
	}, {  /* above = d45 */
		{ 103,  26,  36, 129, 132, 201,  83,  80,  93 },/*left = dc  */
		{  59,  38,  83, 112, 103, 162,  98, 136,  90 },/*left = v   */
		{  62,  30,  23, 158, 200, 207,  59,  57,  50 },/*left = h   */
		{  67,  30,  29,  84,  86, 191, 102,  91,  59 },/*left = d45 */
		{  60,  32,  33, 112,  71, 220,  64,  89, 104 },/*left = d135*/
		{  53,  26,  34, 130,  56, 149,  84, 120, 103 },/*left = d117*/
		{  53,  21,  23, 133, 109, 210,  56,  77, 172 },/*left = d153*/
		{  77,  19,  29, 112, 142, 228,  55,  66,  36 },/*left = d207*/
		{  61,  29,  29,  93,  97, 165,  83, 175, 162 },/*left = d63 */
		{  47,  47,  43, 114, 137, 181, 100,  99,  95 } /*left = tm  */
	}, {  /* above = d135 */
		{  69,  23,  29, 128,  83, 199,  46,  44, 101 },/*left = dc  */
		{  53,  40,  55, 139,  69, 183,  61,  80, 110 },/*left = v   */
		{  40,  29,  19, 161, 180, 207,  43,  24,  91 },/*left = h   */
		{  60,  34,  19, 105,  61, 198,  53,  64,  89 },/*left = d45 */
		{  52,  31,  22, 158,  40, 209,  58,  62,  89 },/*left = d135*/
		{  44,  31,  29, 147,  46, 158,  56, 102, 198 },/*left = d117*/
		{  35,  19,  12, 135,  87, 209,  41,  45, 167 },/*left = d153*/
		{  55,  25,  21, 118,  95, 215,  38,  39,  66 },/*left = d207*/
		{  51,  38,  25, 113,  58, 164,  70,  93,  97 },/*left = d63 */
		{  47,  54,  34, 146, 108, 203,  72, 103, 151 } /*left = tm  */
	}, {  /* above = d117 */
		{  64,  19,  37, 156,  66, 138,  49,  95, 133 },/*left = dc  */
		{  46,  27,  80, 150,  55, 124,  55, 121, 135 },/*left = v   */
		{  36,  23,  27, 165, 149, 166,  54,  64, 118 },/*left = h   */
		{  53,  21,  36, 131,  63, 163,  60, 109,  81 },/*left = d45 */
		{  40,  26,  35, 154,  40, 185,  51,  97, 123 },/*left = d135*/
		{  35,  19,  34, 179,  19,  97,  48, 129, 124 },/*left = d117*/
		{  36,  20,  26, 136,  62, 164,  33,  77, 154 },/*left = d153*/
		{  45,  18,  32, 130,  90, 157,  40,  79,  91 },/*left = d207*/
		{  45,  26,  28, 129,  45, 129,  49, 147, 123 },/*left = d63 */
		{  38,  44,  51, 136,  74, 162,  57,  97, 121 } /*left = tm  */
	}, {  /* above = d153 */
		{  75,  17,  22, 136, 138, 185,  32,  34, 166 },/*left = dc  */
		{  56,  39,  58, 133, 117, 173,  48,  53, 187 },/*left = v   */
		{  35,  21,  12, 161, 212, 207,  20,  23, 145 },/*left = h   */
		{  56,  29,  19, 117, 109, 181,  55,  68, 112 },/*left = d45 */
		{  47,  29,  17, 153,  64, 220,  59,  51, 114 },/*left = d135*/
		{  46,  16,  24, 136,  76, 147,  41,  64, 172 },/*left = d117*/
		{  34,  17,  11, 108, 152, 187,  13,  15, 209 },/*left = d153*/
		{  51,  24,  14, 115, 133, 209,  32,  26, 104 },/*left = d207*/
		{  55,  30,  18, 122,  79, 179,  44,  88, 116 },/*left = d63 */
		{  37,  49,  25, 129, 168, 164,  41,  54, 148 } /*left = tm  */
	}, {  /* above = d207 */
		{  82,  22,  32, 127, 143, 213,  39,  41,  70 },/*left = dc  */
		{  62,  44,  61, 123, 105, 189,  48,  57,  64 },/*left = v   */
		{  47,  25,  17, 175, 222, 220,  24,  30,  86 },/*left = h   */
		{  68,  36,  17, 106, 102, 206,  59,  74,  74 },/*left = d45 */
		{  57,  39,  23, 151,  68, 216,  55,  63,  58 },/*left = d135*/
		{  49,  30,  35, 141,  70, 168,  82,  40, 115 },/*left = d117*/
		{  51,  25,  15, 136, 129, 202,  38,  35, 139 },/*left = d153*/
		{  68,  26,  16, 111, 141, 215,  29,  28,  28 },/*left = d207*/
		{  59,  39,  19, 114,  75, 180,  77, 104,  42 },/*left = d63 */
		{  40,  61,  26, 126, 152, 206,  61,  59,  93 } /*left = tm  */
	}, {  /* above = d63 */
		{  78,  23,  39, 111, 117, 170,  74, 124,  94 },/*left = dc  */
		{  48,  34,  86, 101,  92, 146,  78, 179, 134 },/*left = v   */
		{  47,  22,  24, 138, 187, 178,  68,  69,  59 },/*left = h   */
		{  56,  25,  33, 105, 112, 187,  95, 177, 129 },/*left = d45 */
		{  48,  31,  27, 114,  63, 183,  82, 116,  56 },/*left = d135*/
		{  43,  28,  37, 121,  63, 123,  61, 192, 169 },/*left = d117*/
		{  42,  17,  24, 109,  97, 177,  56,  76, 122 },/*left = d153*/
		{  58,  18,  28, 105, 139, 182,  70,  92,  63 },/*left = d207*/
		{  46,  23,  32,  74,  86, 150,  67, 183,  88 },/*left = d63 */
		{  36,  38,  48,  92, 122, 165,  88, 137,  91 } /*left = tm  */
	}, {  /* above = tm */
		{  65,  70,  60, 155, 159, 199,  61,  60,  81 },/*left = dc  */
		{  44,  78, 115, 132, 119, 173,  71, 112,  93 },/*left = v   */
		{  39,  38,  21, 184, 227, 206,  42,  32,  64 },/*left = h   */
		{  58,  47,  36, 124, 137, 193,  80,  82,  78 },/*left = d45 */
		{  49,  50,  35, 144,  95, 205,  63,  78,  59 },/*left = d135*/
		{  41,  53,  52, 148,  71, 142,  65, 128,  51 },/*left = d117*/
		{  40,  36,  28, 143, 143, 202,  40,  55, 137 },/*left = d153*/
		{  52,  34,  29, 129, 183, 227,  42,  35,  43 },/*left = d207*/
		{  42,  44,  44, 104, 105, 164,  64, 130,  80 },/*left = d63 */
		{  43,  81,  53, 140, 169, 204,  68,  84,  72 } /*left = tm  */
	}
};

static u8 kf_partition_probs[16][3] = {
	/* 8x8 -> 4x4 */
	{ 158,  97,  94 },	/* a/l both not split   */
	{  93,  24,  99 },	/* a split, l not split */
	{  85, 119,  44 },	/* l split, a not split */
	{  62,  59,  67 },	/* a/l both split       */
	/* 16x16 -> 8x8 */
	{ 149,  53,  53 },	/* a/l both not split   */
	{  94,  20,  48 },	/* a split, l not split */
	{  83,  53,  24 },	/* l split, a not split */
	{  52,  18,  18 },	/* a/l both split       */
	/* 32x32 -> 16x16 */
	{ 150,  40,  39 },	/* a/l both not split   */
	{  78,  12,  26 },	/* a split, l not split */
	{  67,  33,  11 },	/* l split, a not split */
	{  24,   7,   5 },	/* a/l both split       */
	/* 64x64 -> 32x32 */
	{ 174,  35,  49 },	/* a/l both not split   */
	{  68,  11,  27 },	/* a split, l not split */
	{  57,  15,   9 },	/* l split, a not split */
	{  12,   3,   3 },	/* a/l both split       */
};

static const u8 kf_uv_mode_prob[10][9] = {
	{ 144,  11,  54, 157, 195, 130,  46,  58, 108 },  /* y = dc   */
	{ 118,  15, 123, 148, 131, 101,  44,  93, 131 },  /* y = v    */
	{ 113,  12,  23, 188, 226, 142,  26,  32, 125 },  /* y = h    */
	{ 120,  11,  50, 123, 163, 135,  64,  77, 103 },  /* y = d45  */
	{ 113,   9,  36, 155, 111, 157,  32,  44, 161 },  /* y = d135 */
	{ 116,   9,  55, 176,  76,  96,  37,  61, 149 },  /* y = d117 */
	{ 115,   9,  28, 141, 161, 167,  21,  25, 193 },  /* y = d153 */
	{ 120,  12,  32, 145, 195, 142,  32,  38,  86 },  /* y = d207 */
	{ 116,  12,  64, 120, 140, 125,  49, 115, 121 },  /* y = d63  */
	{ 102,  19,  66, 162, 182, 122,  35,  59, 128 }   /* y = tm   */
};

static void write_coeff_plane(const u8 coef[6][6][3], u8 *coeff_plane)
{
	unsigned int idx = 0;
	u8 byte_count = 0, p;
	s32 k, m, n;

	for (k = 0; k < 6; k++) {
		for (m = 0; m < 6; m++) {
			for (n = 0; n < 3; n++) {
				p = coef[k][m][n];
				coeff_plane[idx++] = p;
				byte_count++;
				if (byte_count == 27) {
					idx += 5;
					byte_count = 0;
				}
			}
		}
	}
}

static void init_intra_only_probs(struct rkvdec_ctx *ctx,
				  const struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
	struct rkvdec_vp9_intra_only_frame_probs *rkprobs;
	const struct v4l2_vp9_probs *probs;
	unsigned i, j, k, m;

	rkprobs = &tbl->probs.intra_only_probs;
	dec_params = run->decode_params;
	probs = &dec_params->probs;

	/*
	 * intra only 149 x 128 bits ,aligned to 152 x 128 bits coeff related
	 * prob 64 x 128 bits
	 */
	for (i = 0; i < ARRAY_SIZE(probs->coef_probs); i++) {
		for (j = 0; j < ARRAY_SIZE(probs->coef_probs[0]); j++)
			write_coeff_plane(probs->coef_probs[i][j][0],
					  rkprobs->coef_probs_intra[i][j]);
	}

	/* intra mode prob  80 x 128 bits */
	for (i = 0; i < ARRAY_SIZE(vp9_kf_y_mode_prob); i++) {
		u32 byte_count = 0;
		int idx = 0;

		/* vp9_kf_y_mode_prob */
		for (j = 0; j < ARRAY_SIZE(vp9_kf_y_mode_prob[0]); j++) {
			for (k = 0; k < ARRAY_SIZE(vp9_kf_y_mode_prob[0][0]);
			     k++) {
				u8 val = vp9_kf_y_mode_prob[i][j][k];

				rkprobs->intra_mode[i].y_mode_prob[idx++] = val;
				byte_count++;
				if (byte_count == 27) {
					byte_count = 0;
					idx += 5;
				}
			}
		}

		idx = 0;
		if (i < 4) {
			for (m = 0; m < (i < 3 ? 23 : 21); m++) {
				const u8 *ptr = &kf_uv_mode_prob[0][0];

				rkprobs->intra_mode[i].uv_mode_prob[idx++] = ptr[i * 23 + m];
			}
		}
	}
}

static void init_inter_probs(struct rkvdec_ctx *ctx,
			     const struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
	struct rkvdec_vp9_inter_frame_probs *rkprobs;
	const struct v4l2_vp9_probs *probs;
	unsigned i, j, k;

	rkprobs = &tbl->probs.inter_probs;
	dec_params = run->decode_params;
	probs = &dec_params->probs;

	/*
	 * inter probs
	 * 151 x 128 bits, aligned to 152 x 128 bits
	 * inter only
	 * intra_y_mode & inter_block info 6 x 128 bits
	 */

	memcpy(rkprobs->y_mode_probs, probs->y_mode_probs,
	       sizeof(rkprobs->y_mode_probs));
	memcpy(rkprobs->comp_mode_prob, probs->comp_mode_prob,
	       sizeof(rkprobs->comp_mode_prob));
	memcpy(rkprobs->comp_ref_prob, probs->comp_ref_prob,
	       sizeof(rkprobs->comp_ref_prob));
	memcpy(rkprobs->single_ref_prob, probs->single_ref_prob,
	       sizeof(rkprobs->single_ref_prob));
	memcpy(rkprobs->inter_mode_probs, probs->inter_mode_probs,
	       sizeof(rkprobs->inter_mode_probs));
	memcpy(rkprobs->interp_filter_probs, probs->interp_filter_probs,
	       sizeof(rkprobs->interp_filter_probs));

	/* 128 x 128 bits coeff related */
	for (i = 0; i < ARRAY_SIZE(probs->coef_probs); i++) {
		for (j = 0; j < ARRAY_SIZE(probs->coef_probs[0]); j++) {
			for (k = 0; k < ARRAY_SIZE(probs->coef_probs[0][0]); k++)
				write_coeff_plane(probs->coef_probs[i][j][k],
						  rkprobs->coef_probs[k][i][j]);
		}
	}

	/* intra uv mode 6 x 128 */
	memcpy(rkprobs->uv_mode_prob_0_2, probs->uv_mode_probs,
	       sizeof(rkprobs->uv_mode_prob_0_2));
	memcpy(rkprobs->uv_mode_prob_3_5, &probs->uv_mode_probs[3],
	       sizeof(rkprobs->uv_mode_prob_3_5));
	memcpy(rkprobs->uv_mode_prob_6_8, &probs->uv_mode_probs[6],
	       sizeof(rkprobs->uv_mode_prob_6_8));
	memcpy(rkprobs->uv_mode_prob_9, &probs->uv_mode_probs[9],
	       sizeof(rkprobs->uv_mode_prob_9));

	/* mv related 6 x 128 */
	memcpy(rkprobs->mv_joint_probs, probs->mv_joint_probs,
	       sizeof(rkprobs->mv_joint_probs));
	memcpy(rkprobs->mv_sign_prob, probs->mv_sign_prob,
	       sizeof(rkprobs->mv_sign_prob));
	memcpy(rkprobs->mv_class_probs, probs->mv_class_probs,
	       sizeof(rkprobs->mv_class_probs));
	memcpy(rkprobs->mv_class0_bit_prob, probs->mv_class0_bit_prob,
	       sizeof(rkprobs->mv_class0_bit_prob));
	memcpy(rkprobs->mv_bits_prob, probs->mv_bits_prob,
	       sizeof(rkprobs->mv_bits_prob));
	memcpy(rkprobs->mv_class0_fr_probs, probs->mv_class0_fr_probs,
	       sizeof(rkprobs->mv_class0_fr_probs));
	memcpy(rkprobs->mv_fr_probs, probs->mv_fr_probs,
	       sizeof(rkprobs->mv_fr_probs));
	memcpy(rkprobs->mv_class0_hp_prob, probs->mv_class0_hp_prob,
	       sizeof(rkprobs->mv_class0_hp_prob));
	memcpy(rkprobs->mv_hp_prob, probs->mv_hp_prob,
	       sizeof(rkprobs->mv_hp_prob));
}

static void dump_probs(struct rkvdec_vp9_priv_tbl *tbl)
{
	const u8 *raw = (u8 *)tbl;
	unsigned int i;

	return;
	for (i = 0; i < 152; i++)
		pr_info("%08x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
			i * 16,
			raw[i * 16], raw[i * 16 + 1], raw[i * 16 + 2], raw[i * 16 + 3],
			raw[i * 16 + 4], raw[i * 16 + 5], raw[i * 16 + 6], raw[i * 16 + 7],
			raw[i * 16 + 8], raw[i * 16 + 9], raw[i * 16 + 10], raw[i * 16 + 11],
                        raw[i * 16 + 12], raw[i * 16 + 13], raw[i * 16 + 14], raw[i * 16 + 15]);
}

static void dump_regs(struct rkvdec_dev *rkvdec)
{
	unsigned int i;

	return;
	for (i = 0; i < 0xe0; i += 4)
		pr_info("%08x  %08x\n", i, readl(rkvdec->regs + i));
}

static void init_probs(struct rkvdec_ctx *ctx,
		       const struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
	struct rkvdec_vp9_probs *rkprobs = &tbl->probs;
	const struct v4l2_vp9_segmentation *seg;
	const struct v4l2_vp9_probs *probs;
	bool intra_only;

	dec_params = run->decode_params;
	probs = &dec_params->probs;
	seg = &dec_params->seg;

	memset(rkprobs, 0, sizeof(*rkprobs));

	intra_only = !!(dec_params->flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	/* sb info  5 x 128 bit */
	memcpy(rkprobs->partition_probs,
	       intra_only ? kf_partition_probs : probs->partition_probs,
	       sizeof(rkprobs->partition_probs));

	memcpy(rkprobs->pred_probs, seg->pred_probs, sizeof(rkprobs->pred_probs));
	memcpy(rkprobs->tree_probs, seg->tree_probs, sizeof(rkprobs->tree_probs));
	memcpy(rkprobs->skip_prob, probs->skip_prob,
	       sizeof(rkprobs->skip_prob));
	memcpy(rkprobs->tx_probs_32x32, probs->tx_probs_32x32,
	       sizeof(rkprobs->tx_probs_32x32));
	memcpy(rkprobs->tx_probs_16x16, probs->tx_probs_16x16,
	       sizeof(rkprobs->tx_probs_16x16));
	memcpy(rkprobs->tx_probs_8x8, probs->tx_probs_8x8,
	       sizeof(rkprobs->tx_probs_8x8));
	memcpy(rkprobs->is_inter_prob, probs->is_inter_prob,
               sizeof(rkprobs->is_inter_prob));

	if (intra_only)
		init_intra_only_probs(ctx, run);
	else
		init_inter_probs(ctx, run);

	dump_probs(tbl);
}

struct vp9d_ref_config {
	u32 reg_frm_size;
	u32 reg_hor_stride;
	u32 reg_y_stride;
	u32 reg_yuv_stride;
	u32 reg_ref_base;
};

static struct vp9d_ref_config ref_config[3] = {
	{
		.reg_frm_size = RKVDEC_REG_VP9_FRAME_SIZE(0),
		.reg_hor_stride = RKVDEC_VP9_HOR_VIRSTRIDE(0),
		.reg_y_stride = RKVDEC_VP9_LAST_FRAME_YSTRIDE,
		.reg_yuv_stride = RKVDEC_VP9_LAST_FRAME_YUVSTRIDE,
		.reg_ref_base = RKVDEC_REG_VP9_LAST_FRAME_BASE,
	},
	{
		.reg_frm_size = RKVDEC_REG_VP9_FRAME_SIZE(1),
		.reg_hor_stride = RKVDEC_VP9_HOR_VIRSTRIDE(1),
		.reg_y_stride = RKVDEC_VP9_GOLDEN_FRAME_YSTRIDE,
		.reg_yuv_stride = 0,
		.reg_ref_base = RKVDEC_REG_VP9_GOLDEN_FRAME_BASE,
	},
	{
		.reg_frm_size = RKVDEC_REG_VP9_FRAME_SIZE(2),
		.reg_hor_stride = RKVDEC_VP9_HOR_VIRSTRIDE(2),
		.reg_y_stride = RKVDEC_VP9_ALTREF_FRAME_YSTRIDE,
		.reg_yuv_stride = 0,
		.reg_ref_base = RKVDEC_REG_VP9_ALTREF_FRAME_BASE,
	}
};

static struct rkvdec_decoded_buffer *
get_ref_buf(struct rkvdec_ctx *ctx,
	    const struct v4l2_ctrl_vp9_frame_decode_params *dec_params,
	    struct vb2_v4l2_buffer *dst,
	    enum v4l2_vp9_ref_id id)
{
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct vb2_queue *cap_q = &m2m_ctx->cap_q_ctx.q;
	int buf_idx;

	/*
	 * If a ref is unused or invalid, address of current destination
	 * buffer is returned.
	 */
	buf_idx = vb2_find_timestamp(cap_q, dec_params->refs[id], 0);
	if (buf_idx < 0)
		return vb2_to_rkvdec_decoded_buf(&dst->vb2_buf);

	return vb2_to_rkvdec_decoded_buf(vb2_get_buffer(cap_q, buf_idx));
}

static dma_addr_t get_mv_base_addr(struct rkvdec_decoded_buffer *buf)
{
	u32 aligned_pitch, aligned_height, yuv_len, width, height;

	width = buf->vp9.params.frame_width_minus_1 + 1;
	height = buf->vp9.params.frame_height_minus_1 + 1;
	aligned_height = round_up(height, 64);
	aligned_pitch = round_up(width * buf->vp9.params.bit_depth, 512) / 8;
	yuv_len = (aligned_height * aligned_pitch * 3) / 2;

	return vb2_dma_contig_plane_dma_addr(&buf->base.vb.vb2_buf, 0) +
	       yuv_len;
}

static void
config_ref_registers(struct rkvdec_ctx *ctx,
		     const struct rkvdec_vp9_run *run,
		     struct rkvdec_decoded_buffer **ref_bufs,
		     enum v4l2_vp9_ref_id id)
{
	u32 width, height, aligned_pitch, aligned_height, y_len, yuv_len;
	struct rkvdec_decoded_buffer *buf = ref_bufs[id];
	struct rkvdec_dev *rkvdec = ctx->dev;

	width = buf->vp9.params.frame_width_minus_1 + 1;
	height = buf->vp9.params.frame_height_minus_1 + 1;
	aligned_height = round_up(height, 64);
	writel_relaxed(RKVDEC_VP9_FRAMEWIDTH(round_up(width, 64)) |
		       RKVDEC_VP9_FRAMEHEIGHT(height),
		       rkvdec->regs + ref_config[id].reg_frm_size);

	writel_relaxed(vb2_dma_contig_plane_dma_addr(&buf->base.vb.vb2_buf, 0),
		       rkvdec->regs + ref_config[id].reg_ref_base);

	if (&buf->base.vb == run->base.bufs.dst)
		return;

	aligned_pitch = round_up(width * buf->vp9.params.bit_depth, 512) / 8;
	y_len = aligned_height * aligned_pitch;
	yuv_len = (y_len * 3) / 2;

	writel_relaxed(RKVDEC_HOR_Y_VIRSTRIDE(aligned_pitch / 16) |
		       RKVDEC_HOR_UV_VIRSTRIDE(aligned_pitch / 16),
		       rkvdec->regs + ref_config[id].reg_hor_stride);
	writel_relaxed(RKVDEC_VP9_REF_YSTRIDE(y_len / 16),
		       rkvdec->regs + ref_config[id].reg_y_stride);

	if (!ref_config[id].reg_yuv_stride)
		return;

	writel_relaxed(RKVDEC_VP9_REF_YUVSTRIDE(yuv_len / 16),
		      rkvdec->regs + ref_config[id].reg_yuv_stride);
}

static bool seg_featured_enabled(const struct rkvdec_decoded_buffer *buf,
				 enum v4l2_vp9_segmentation_feature feature,
				 unsigned int segid)
{
	u8 mask = V4L2_VP9_SEGMENTATION_FEATURE_ENABLED(feature);

	return !!(buf->vp9.params.seg.feature_enabled[segid] & mask);
}

static void
config_seg_registers(struct rkvdec_ctx *ctx,
		     struct rkvdec_decoded_buffer *last,
		     unsigned int segid)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	s16 feature_val;
	u8 feature_id;
	u32 val = 0;

	feature_id = V4L2_VP9_SEGMENTATION_FEATURE_QP_DELTA;
	if (seg_featured_enabled(last, feature_id, segid)) {
		feature_val = last->vp9.params.seg.feature_data[segid][feature_id];
		val |= RKVDEC_SEGID_FRAME_QP_DELTA_EN(1) |
		       RKVDEC_SEGID_FRAME_QP_DELTA(feature_val);
	}
	
	feature_id = V4L2_VP9_SEGMENTATION_FEATURE_LF_VAL;
	if (seg_featured_enabled(last, feature_id, segid)) {
		feature_val = last->vp9.params.seg.feature_data[segid][feature_id];
		val |= RKVDEC_SEGID_FRAME_LOOPFILTER_VALUE_EN(1) |
		       RKVDEC_SEGID_FRAME_LOOPFILTER_VALUE(feature_val);
	}

	feature_id = V4L2_VP9_SEGMENTATION_FEATURE_REFERINFO;
	if (seg_featured_enabled(last, feature_id, segid)) {
		feature_val = last->vp9.params.seg.feature_data[segid][feature_id];
		val |= RKVDEC_SEGID_REFERINFO_EN(1) |
		       RKVDEC_SEGID_REFERINFO(feature_val);
	}

	feature_id = V4L2_VP9_SEGMENTATION_FEATURE_FRAME_SKIP;
	if (seg_featured_enabled(last, feature_id, segid))
		val |= RKVDEC_SEGID_FRAME_SKIP_EN(1);

	if (!segid &&
	    (last->vp9.params.seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE))
		val |= RKVDEC_SEGID_ABS_DELTA(1);

	writel_relaxed(val, rkvdec->regs + RKVDEC_VP9_SEGID_GRP(segid));
}

static void config_registers(struct rkvdec_ctx *ctx,
			     const struct rkvdec_vp9_run *run)
{
	struct rkvdec_decoded_buffer *ref_bufs[V4L2_REF_ID_CNT], *dst, *last;
	u32 y_len, uv_len, yuv_len, bit_depth, aligned_height, aligned_pitch;
	const struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	u32 val, stream_len, last_frame_info = 0;
	const struct v4l2_vp9_segmentation *seg;
	struct rkvdec_dev *rkvdec = ctx->dev;
	dma_addr_t addr;
	bool intra_only;
	unsigned int i;

	dec_params = run->decode_params;
	dst = vb2_to_rkvdec_decoded_buf(&run->base.bufs.dst->vb2_buf);
	for (i = 0; i < ARRAY_SIZE(ref_bufs); i++)
		ref_bufs[i] = get_ref_buf(ctx, dec_params, &dst->base.vb, i);

	last = ref_bufs[V4L2_REF_ID_LAST];
	dst->vp9.params = *dec_params;
	seg = &dec_params->seg;

	intra_only = !!(dec_params->flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	writel_relaxed(RKVDEC_MODE(RKVDEC_MODE_VP9),
		       rkvdec->regs + RKVDEC_REG_SYSCTRL);

	bit_depth = dec_params->bit_depth;
	aligned_height = round_up(ctx->decoded_fmt.fmt.pix_mp.height, 64);

	aligned_pitch = round_up(ctx->decoded_fmt.fmt.pix_mp.width *
				 bit_depth,
				 512) / 8;
	y_len = aligned_height * aligned_pitch;
	uv_len = y_len / 2;
	yuv_len = y_len + uv_len;

	writel_relaxed(RKVDEC_Y_HOR_VIRSTRIDE(aligned_pitch / 16) |
		       RKVDEC_UV_HOR_VIRSTRIDE(aligned_pitch / 16),
		       rkvdec->regs + RKVDEC_REG_PICPAR);
	writel_relaxed(RKVDEC_Y_VIRSTRIDE(y_len / 16),
		       rkvdec->regs + RKVDEC_REG_Y_VIRSTRIDE);
	writel_relaxed(RKVDEC_YUV_VIRSTRIDE(yuv_len / 16),
		       rkvdec->regs + RKVDEC_REG_YUV_VIRSTRIDE);

	stream_len = vb2_get_plane_payload(&run->base.bufs.src->vb2_buf, 0);
	writel_relaxed(RKVDEC_STRM_LEN(stream_len),
		       rkvdec->regs + RKVDEC_REG_STRM_LEN);

	/*
	 * Reset count buffer, because decoder only output intra related syntax
	 * counts when decoding intra frame, but update entropy need to update
	 * all the probabilities.
	 */
	if (intra_only)
		memset(vp9_ctx->count_tbl.cpu, 0, vp9_ctx->count_tbl.size);

	dst->vp9.segmapid = ref_bufs[V4L2_REF_ID_LAST]->vp9.segmapid;
	if (!intra_only &&
	    !(dec_params->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
	    (!(seg->flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED) ||
	     (seg->flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP)))
		dst->vp9.segmapid++;

	for (i = 0; i < ARRAY_SIZE(ref_bufs); i++)
		config_ref_registers(ctx, run, ref_bufs, i);

	for (i = 0; i < 8; i++)
		config_seg_registers(ctx, ref_bufs[V4L2_REF_ID_LAST], i);

	writel_relaxed(RKVDEC_VP9_TX_MODE(dec_params->tx_mode) |
		       RKVDEC_VP9_FRAME_REF_MODE(dec_params->reference_mode),
		       rkvdec->regs + RKVDEC_VP9_CPRHEADER_CONFIG);

	if (!intra_only) {
		s8 delta;

		val = 0;
		for (i = 0; i < ARRAY_SIZE(last->vp9.params.lf.ref_deltas); i++) {
			delta = last->vp9.params.lf.ref_deltas[i];
			val |= RKVDEC_REF_DELTAS_LASTFRAME(i, delta);
		}

		writel_relaxed(val,
			       rkvdec->regs + RKVDEC_VP9_REF_DELTAS_LASTFRAME);

		for (i = 0; i < ARRAY_SIZE(last->vp9.params.lf.mode_deltas); i++) {
			delta = last->vp9.params.lf.mode_deltas[i];
			last_frame_info |= RKVDEC_MODE_DELTAS_LASTFRAME(i,
									delta);
		}
	}

	if (last != dst && !intra_only && last->vp9.params.seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED)
		last_frame_info |= RKVDEC_SEG_EN_LASTFRAME;

	if (last != dst && last->vp9.params.flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME)
		last_frame_info |= RKVDEC_LAST_SHOW_FRAME;

	if (last != dst &&
	    last->vp9.params.flags &
	    (V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_INTRA_ONLY))
		last_frame_info |= RKVDEC_LAST_INTRA_ONLY;

	if (last != dst &&
	    dec_params->frame_width_minus_1 == last->vp9.params.frame_width_minus_1 &&
	    dec_params->frame_height_minus_1 == last->vp9.params.frame_height_minus_1)
		last_frame_info |= RKVDEC_LAST_WIDHHEIGHT_EQCUR;

	writel_relaxed(last_frame_info,
		       rkvdec->regs + RKVDEC_VP9_INFO_LASTFRAME);

	writel_relaxed(stream_len /* - dec_params->header_size_in_bytes*/,
		       rkvdec->regs + RKVDEC_VP9_LASTTILE_SIZE);

	for (i = 0; !intra_only && i < ARRAY_SIZE(ref_bufs); i++) {
		u32 refw = ref_bufs[i]->vp9.params.frame_width_minus_1 + 1;
		u32 refh = ref_bufs[i]->vp9.params.frame_height_minus_1 + 1;
		u32 hscale, vscale;

		hscale = (refw << 14) /	(dec_params->frame_width_minus_1 + 1);
		vscale = (refh << 14) / (dec_params->frame_height_minus_1 + 1);
		writel_relaxed(RKVDEC_VP9_REF_HOR_SCALE(hscale) |
			       RKVDEC_VP9_REF_VER_SCALE(vscale),
			       rkvdec->regs + RKVDEC_VP9_REF_SCALE(i));
	}

	addr = vb2_dma_contig_plane_dma_addr(&dst->base.vb.vb2_buf, 0);
	writel_relaxed(addr, rkvdec->regs + RKVDEC_REG_DECOUT_BASE);
	addr = vb2_dma_contig_plane_dma_addr(&run->base.bufs.src->vb2_buf, 0);
	writel_relaxed(addr, rkvdec->regs + RKVDEC_REG_STRM_RLC_BASE);
	writel_relaxed(vp9_ctx->priv_tbl.dma +
		       offsetof(struct rkvdec_vp9_priv_tbl, probs),
		       rkvdec->regs + RKVDEC_REG_CABACTBL_PROB_BASE);
	writel_relaxed(vp9_ctx->count_tbl.dma,
		       rkvdec->regs + RKVDEC_REG_VP9COUNT_BASE);

	writel_relaxed(vp9_ctx->priv_tbl.dma +
		       offsetof(struct rkvdec_vp9_priv_tbl, segmap) +
		       (RKVDEC_VP9_MAX_SEGMAP_SIZE * dst->vp9.segmapid),
		       rkvdec->regs + RKVDEC_REG_VP9_SEGIDCUR_BASE);
	writel_relaxed(vp9_ctx->priv_tbl.dma +
		       offsetof(struct rkvdec_vp9_priv_tbl, segmap) +
		       (RKVDEC_VP9_MAX_SEGMAP_SIZE * (!dst->vp9.segmapid)),
		       rkvdec->regs + RKVDEC_REG_VP9_SEGIDLAST_BASE);

	if (!intra_only &&
	    !(dec_params->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT))
		addr = get_mv_base_addr(ref_bufs[V4L2_REF_ID_LAST]);
	else
		addr = get_mv_base_addr(dst);

	writel_relaxed(addr, rkvdec->regs + RKVDEC_VP9_REF_COLMV_BASE);

	writel_relaxed(ctx->decoded_fmt.fmt.pix_mp.width |
		       (ctx->decoded_fmt.fmt.pix_mp.height << 16),
		       rkvdec->regs + RKVDEC_REG_PERFORMANCE_CYCLE);
	dump_regs(rkvdec);
}

static void rkvdec_vp9_run_preamble(struct rkvdec_ctx *ctx,
				    struct rkvdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame_ctx *fctx = NULL;
	struct rkvdec_decoded_buffer *dst;
	struct v4l2_ctrl *ctrl;
	u8 frm_ctx;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			       V4L2_CID_MPEG_VIDEO_VP9_FRAME_DECODE_PARAMS);
	WARN_ON(!ctrl);
	run->decode_params = ctrl ? ctrl->p_cur.p : NULL;
	if (WARN_ON(!run->decode_params))
		return;

	frm_ctx = run->decode_params->frame_context_idx;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_VP9_FRAME_CONTEXT(frm_ctx));
	if (ctrl)
		fctx = ctrl->p_cur.p;

	rkvdec_run_preamble(ctx, &run->base);
	dst = vb2_to_rkvdec_decoded_buf(&run->base.bufs.dst->vb2_buf);
	if (fctx)
		dst->vp9.frame_context = *fctx;
	else
		dst->vp9.frame_context.probs = run->decode_params->probs;
}

static void rkvdec_vp9_run(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_vp9_run run = { };

	rkvdec_vp9_run_preamble(ctx, &run);

	if (WARN_ON(!run.decode_params))
		return;

	/* Prepare probs. */
	init_probs(ctx, &run);

	/* Configure hardware registers. */
	config_registers(ctx, &run);

	rkvdec_run_postamble(ctx, &run.base);

	schedule_delayed_work(&rkvdec->watchdog_work, msecs_to_jiffies(2000));

	writel(1, rkvdec->regs + RKVDEC_REG_PREF_LUMA_CACHE_COMMAND);
	writel(1, rkvdec->regs + RKVDEC_REG_PREF_CHR_CACHE_COMMAND);

	/* Start decoding! */
	writel(RKVDEC_INTERRUPT_DEC_E | RKVDEC_CONFIG_DEC_CLK_GATE_E |
	       RKVDEC_TIMEOUT_E,
	       rkvdec->regs + RKVDEC_REG_INTERRUPT);
}

static u8 adapt_prob(u8 p1, u32 ct0, u32 ct1, u16 max_count, u32 update_factor)
{
	u32 ct = ct0 + ct1, p2;
	u32 lo = 1;
	u32 hi = 255;

	if (!ct)
		return p1;

	p2 = ((ct0 << 8) + (ct >> 1)) / ct;
	p2 = clamp(p2, lo, hi);
	ct = min_t(u32, ct, max_count);

	if (WARN_ON(max_count >= 257))
		return p1;

	update_factor = FASTDIV(update_factor * ct, max_count);

	return p1 + (((p2 - p1) * update_factor + 128) >> 8);
}

#define BAND_6(band) ((band) == 0 ? 3 : 6)

static void adapt_coeff(const u8 pre_coef_probs[6][6][3],
			u8 coef_probs[6][6][3],
			const struct rkvdec_vp9_refs_counts ref_cnt[6][6],
			u32 uf)
{
	s32 l, m, n;

	for (l = 0; l < 6; l++) {
		for (m = 0; m < BAND_6(l); m++) {
			const u8 *pp = pre_coef_probs[l][m];
			u8 *p = coef_probs[l][m];
			const u32 n0 = ref_cnt[l][m].coeff[0];
			const u32 n1 = ref_cnt[l][m].coeff[1];
			const u32 n2 = ref_cnt[l][m].coeff[2];
			const u32 neob = ref_cnt[l][m].eob[1];
			const u32 eob_count = ref_cnt[l][m].eob[0];
			const u32 branch_ct[3][2] = {
				{ neob, eob_count - neob },
				{ n0, n1 + n2 },
				{ n1, n2 }
			};

			for (n = 0; n < 3; n++)
				p[n] = adapt_prob(pp[n], branch_ct[n][0],
						  branch_ct[n][1], 24, uf);
		}
	}
}

static void
adapt_coef_probs(const struct v4l2_vp9_probs *orig,
		 struct v4l2_vp9_probs *cur,
		 const struct rkvdec_vp9_refs_counts ref_cnt[2][4][2][6][6],
		 unsigned int uf)
{
	unsigned int i, j, k;

	for (i = 0; i < ARRAY_SIZE(orig->coef_probs); i++) {
		for (j = 0; j < ARRAY_SIZE(orig->coef_probs[0]); j++) {
			for (k = 0; k < ARRAY_SIZE(orig->coef_probs[0][0]);
			     k++) {
				adapt_coeff(orig->coef_probs[i][j][k],
					    cur->coef_probs[i][j][k],
					    ref_cnt[k][i][j],
					    uf);
			}
		}
	}
}

static void adapt_intra_frame_probs(const struct v4l2_vp9_probs *orig,
				    struct v4l2_vp9_probs *cur,
				    const void *count_tbl)
{
	const struct rkvdec_vp9_intra_frame_symbol_counts *sym_cnts = count_tbl;

	adapt_coef_probs(orig, cur, sym_cnts->ref_cnt, 112);
}

static void
adapt_skip_probs(const struct v4l2_vp9_probs *orig,
		 struct v4l2_vp9_probs *cur,
		 const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->skip_prob); i++)
		cur->skip_prob[i] = adapt_prob(orig->skip_prob[i],
					       sym_cnts->skip[i][0],
					       sym_cnts->skip[i][1], 20, 128);
}

static void
adapt_is_inter_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->is_inter_prob); i++)
		cur->is_inter_prob[i] = adapt_prob(orig->is_inter_prob[i],
						   sym_cnts->inter[i][0],
						   sym_cnts->inter[i][1],
						   20, 128);
}

static void
adapt_comp_mode_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->comp_mode_prob); i++)
		cur->comp_mode_prob[i] = adapt_prob(orig->comp_mode_prob[i],
						    sym_cnts->comp[i][0],
						    sym_cnts->comp[i][1],
						    20, 128);
}

static void
adapt_comp_ref_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->comp_ref_prob); i++)
		cur->comp_ref_prob[i] =	adapt_prob(orig->comp_ref_prob[i],
						   sym_cnts->comp_ref[i][0],
						   sym_cnts->comp_ref[i][1],
						   20, 128);
}

static void
adapt_single_ref_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->single_ref_prob); i++) {
		const u8 *pp = orig->single_ref_prob[i];
		u8 *p = cur->single_ref_prob[i];

		p[0] = adapt_prob(pp[0], sym_cnts->single_ref[i][0][0],
				  sym_cnts->single_ref[i][0][1], 20, 128);
		p[1] = adapt_prob(pp[1], sym_cnts->single_ref[i][1][0],
				  sym_cnts->single_ref[i][1][1], 20, 128);
	}
}

static void
adapt_partition_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->partition_probs); i++) {
		const u8 *pp = orig->partition_probs[i];
		const u32 *c = sym_cnts->partition[i];
		u8 *p = cur->partition_probs[i];

		p[0] = adapt_prob(pp[0], c[0], c[1] + c[2] + c[3], 20, 128);
		p[1] = adapt_prob(pp[1], c[1], c[2] + c[3], 20, 128);
		p[2] = adapt_prob(pp[2], c[2], c[3], 20, 128);
	}
}

static void
adapt_tx_probs(const struct v4l2_vp9_probs *orig,
	       struct v4l2_vp9_probs *cur,
	       const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->tx_probs_8x8); i++) {
		u8 *cur_16x16 = cur->tx_probs_16x16[i];
		u8 *cur_32x32 = cur->tx_probs_32x32[i];
		const u32 *c16 = sym_cnts->tx16p[i];
		const u32 *c32 = sym_cnts->tx32p[i];
		u8 *cur_8x8 = cur->tx_probs_8x8[i];

		cur_8x8[0] = adapt_prob(orig->tx_probs_8x8[i][0],
					sym_cnts->tx8p[i][0],
					sym_cnts->tx8p[i][1],
					20, 128);
		cur_16x16[0] = adapt_prob(orig->tx_probs_16x16[i][0],
					  c16[0], c16[1] + c16[2], 20, 128);
		cur_16x16[1] = adapt_prob(orig->tx_probs_16x16[i][1],
					  c16[1], c16[2], 20, 128);
		cur_32x32[0] = adapt_prob(orig->tx_probs_32x32[i][0],
					  c32[0], c32[1] + c32[2] + c32[3],
					  20, 128);
		cur_32x32[1] = adapt_prob(orig->tx_probs_32x32[i][1],
					  c32[1], c32[2] + c32[3], 20, 128);
		cur_32x32[2] = adapt_prob(orig->tx_probs_32x32[i][2],
					  c32[2], c32[3], 20, 128);
	}
}

static void
adapt_interp_filter_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->interp_filter_probs); i++) {
		const u8 *pp = orig->interp_filter_probs[i];
		u8 *p = cur->interp_filter_probs[i];
		const u32 *c = sym_cnts->filter[i];

		p[0] = adapt_prob(pp[0], c[0], c[1] + c[2], 20, 128);
		p[1] = adapt_prob(pp[1], c[1], c[2], 20, 128);
	}
}

static void
adapt_inter_mode_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->inter_mode_probs); i++) {
		const u8 *pp = orig->inter_mode_probs[i];
		const u32 *c = sym_cnts->mv_mode[i];
		u8 *p = cur->inter_mode_probs[i];

		p[0] = adapt_prob(pp[0], c[2], c[1] + c[0] + c[3], 20, 128);
		p[1] = adapt_prob(pp[1], c[0], c[1] + c[3], 20, 128);
		p[2] = adapt_prob(pp[2], c[1], c[3], 20, 128);
	}
}

static void
adapt_mv_probs(const struct v4l2_vp9_probs *orig,
	       struct v4l2_vp9_probs *cur,
	       const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts,
	       bool high_prec_mv)
{
	const u8 *pp = orig->mv_joint_probs;
	const u32 *c = sym_cnts->mv_joint;
	u8 *p = cur->mv_joint_probs;
	unsigned int i, j;
	u32 sum;

	p[0] = adapt_prob(pp[0], c[0], c[1] + c[2] + c[3], 20, 128);
	p[1] = adapt_prob(pp[1], c[1], c[2] + c[3], 20, 128);
	p[2] = adapt_prob(pp[2], c[2], c[3], 20, 128);

	for (i = 0; i < ARRAY_SIZE(orig->mv_sign_prob); i++) {
		pp = orig->mv_sign_prob;
		p = cur->mv_sign_prob;

		p[i] = adapt_prob(pp[i], sym_cnts->sign[i][0],
				  sym_cnts->sign[i][1], 20, 128);

		pp = orig->mv_class_probs[i];
		p = cur->mv_class_probs[i];
		c = sym_cnts->classes[i];
		sum = c[1] + c[2] + c[3] + c[4] + c[5] + c[6] + c[7] + c[8] +
		      c[9] + c[10];
		p[0] = adapt_prob(pp[0], c[0], sum, 20, 128);
		sum -= c[1];
		p[1] = adapt_prob(pp[1], c[1], sum, 20, 128);
		sum -= c[2] + c[3];
		p[2] = adapt_prob(pp[2], c[2] + c[3], sum, 20, 128);
		p[3] = adapt_prob(pp[3], c[2], c[3], 20, 128);
		sum -= c[4] + c[5];
		p[4] = adapt_prob(pp[4], c[4] + c[5], sum, 20, 128);
		p[5] = adapt_prob(pp[5], c[4], c[5], 20, 128);
		sum -= c[6];
		p[6] = adapt_prob(pp[6], c[6], sum, 20, 128);
		p[7] = adapt_prob(pp[7], c[7] + c[8], c[9] + c[10], 20, 128);
		p[8] = adapt_prob(pp[8], c[7], c[8], 20, 128);
		p[9] = adapt_prob(pp[9], c[9], c[10], 20, 128);

		pp = orig->mv_class0_bit_prob;
		p = cur->mv_class0_bit_prob;
		p[i] = adapt_prob(pp[i],
				  sym_cnts->class0[i][0],
				  sym_cnts->class0[i][1], 20, 128);

		pp = orig->mv_bits_prob[i];
		p = cur->mv_bits_prob[i];
		for (j = 0; j < 10; j++)
			p[j] = adapt_prob(pp[j], sym_cnts->bits[i][j][0],
					  sym_cnts->bits[i][j][1], 20, 128);

		for (j = 0; j < 2; j++) {
			pp = orig->mv_class0_fr_probs[i][j];
			p = cur->mv_class0_fr_probs[i][j];
			c = sym_cnts->class0_fp[i][j];
			p[0] = adapt_prob(pp[0], c[0], c[1] + c[2] + c[3],
					  20, 128);
			p[1] = adapt_prob(pp[1], c[1], c[2] + c[3], 20, 128);
			p[2] = adapt_prob(pp[2], c[2], c[3], 20, 128);
		}

		pp = orig->mv_fr_probs[i];
		p = cur->mv_fr_probs[i];
		c = sym_cnts->fp[i];
		p[0] = adapt_prob(pp[0], c[0], c[1] + c[2] + c[3], 20, 128);
		p[1] = adapt_prob(pp[1], c[1], c[2] + c[3], 20, 128);
		p[2] = adapt_prob(pp[2], c[2], c[3], 20, 128);

		if (!high_prec_mv)
			continue;

		pp = orig->mv_class0_hp_prob;
		p = cur->mv_class0_hp_prob;
		p[i] = adapt_prob(pp[i], sym_cnts->class0_hp[i][0],
				  sym_cnts->class0_hp[i][1], 20, 128);

		pp = orig->mv_hp_prob;
		p = cur->mv_hp_prob;
		p[i] = adapt_prob(pp[i], sym_cnts->hp[i][0],
				  sym_cnts->hp[i][1], 20, 128);
	}
}

static void
adapt_intra_mode_probs(const u8 *pp, u8 *p, const u32 *c)
{
	u32 sum = 0, s2;
	unsigned int i;

	for (i = V4L2_VP9_INTRA_PRED_MODE_V; i <= V4L2_VP9_INTRA_PRED_MODE_TM;
	     i++)
		sum += c[i];

	p[0] = adapt_prob(pp[0], c[V4L2_VP9_INTRA_PRED_MODE_DC], sum, 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_TM];
	p[1] = adapt_prob(pp[1], c[V4L2_VP9_INTRA_PRED_MODE_TM], sum, 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_V];
	p[2] = adapt_prob(pp[2], c[V4L2_VP9_INTRA_PRED_MODE_V], sum, 20, 128);
	s2 = c[V4L2_VP9_INTRA_PRED_MODE_H] + c[V4L2_VP9_INTRA_PRED_MODE_D135] +
	     c[V4L2_VP9_INTRA_PRED_MODE_D117];
	sum -= s2;
	p[3] = adapt_prob(pp[3], s2, sum, 20, 128);
	s2 -= c[V4L2_VP9_INTRA_PRED_MODE_H];
	p[4] = adapt_prob(pp[4], c[V4L2_VP9_INTRA_PRED_MODE_H], s2, 20, 128);
	p[5] = adapt_prob(pp[5], c[V4L2_VP9_INTRA_PRED_MODE_D135],
			  c[V4L2_VP9_INTRA_PRED_MODE_D117], 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_D45];
	p[6] = adapt_prob(pp[6], c[V4L2_VP9_INTRA_PRED_MODE_D45],
			  sum, 20, 128);
	sum -= c[V4L2_VP9_INTRA_PRED_MODE_D63];
	p[7] = adapt_prob(pp[7], c[V4L2_VP9_INTRA_PRED_MODE_D63], sum,
			  20, 128);
	p[8] = adapt_prob(pp[8], c[V4L2_VP9_INTRA_PRED_MODE_D153],
			  c[V4L2_VP9_INTRA_PRED_MODE_D207], 20, 128);
}

static void
adapt_y_intra_mode_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->y_mode_probs); i++)
		adapt_intra_mode_probs(orig->y_mode_probs[i],
				       cur->y_mode_probs[i],
				       sym_cnts->y_mode[i]);
}

static void
adapt_uv_intra_mode_probs(const struct v4l2_vp9_probs *orig,
		struct v4l2_vp9_probs *cur,
		const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(orig->uv_mode_probs); i++)
		adapt_intra_mode_probs(orig->uv_mode_probs[i],
				       cur->uv_mode_probs[i],
				       sym_cnts->uv_mode[i]);
}

static void
adapt_inter_frame_probs(struct rkvdec_ctx *ctx,
			struct v4l2_ctrl_vp9_frame_decode_params *dec_params,
			struct rkvdec_decoded_buffer *dst,
			const struct v4l2_vp9_probs *orig,
			struct v4l2_vp9_probs *cur,
			const void *count_tbl)
{
	const struct rkvdec_vp9_inter_frame_symbol_counts *sym_cnts = count_tbl;
	struct rkvdec_decoded_buffer *last;

	/* coefficients */
	last = get_ref_buf(ctx, dec_params, &dst->base.vb, V4L2_REF_ID_LAST);
	if (last != dst && !(last->vp9.params.flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME))
		adapt_coef_probs(orig, cur, sym_cnts->ref_cnt, 112);
	else
		adapt_coef_probs(orig, cur, sym_cnts->ref_cnt, 128);

	/* skip flag */
	adapt_skip_probs(orig, cur, sym_cnts);

	/* intra/inter flag */
	adapt_is_inter_probs(orig, cur, sym_cnts);

	/* comppred flag */
	adapt_comp_mode_probs(orig, cur, sym_cnts);

	/* reference frames */
	adapt_comp_ref_probs(orig, cur, sym_cnts);

	if (dec_params->reference_mode != V4L2_VP9_REF_MODE_COMPOUND)
		adapt_single_ref_probs(orig, cur, sym_cnts);

	/* block partitioning */
	adapt_partition_probs(orig, cur, sym_cnts);

	/* tx size */
	if (dec_params->tx_mode == V4L2_VP9_TX_MODE_SELECT)
		adapt_tx_probs(orig, cur, sym_cnts);

	/* interpolation filter */
	if (dec_params->interpolation_filter == V4L2_VP9_INTERP_FILTER_SWITCHABLE)
		adapt_interp_filter_probs(orig, cur, sym_cnts);

	/* inter modes */
	adapt_inter_mode_probs(orig, cur, sym_cnts);

	/* mv probs */
	adapt_mv_probs(orig, cur, sym_cnts,
		       !!(dec_params->flags &
			  V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV));

	/* y intra modes */
	adapt_y_intra_mode_probs(orig, cur, sym_cnts);

	/* uv intra modes */
	adapt_uv_intra_mode_probs(orig, cur, sym_cnts);
}

static void adapt_probs(struct rkvdec_ctx *ctx,
			struct rkvdec_decoded_buffer *dst,
			const void *count_tbl)
{
	struct v4l2_ctrl_vp9_frame_decode_params *dec_params = &dst->vp9.params;
	const struct v4l2_ctrl_vp9_frame_ctx *fctx = &dst->vp9.frame_context;
	const struct v4l2_vp9_probs *orig;
	struct v4l2_vp9_probs *cur;
	bool intra_only;

	orig = &fctx->probs;
	cur = &dec_params->probs;

	intra_only = !!(dec_params->flags &
			(V4L2_VP9_FRAME_FLAG_KEY_FRAME |
			 V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	if (intra_only)
		adapt_intra_frame_probs(orig, cur, count_tbl);
	else
		adapt_inter_frame_probs(ctx, dec_params, dst, orig, cur, count_tbl);


}

static void rkvdec_vp9_done(struct rkvdec_ctx *ctx,
			    struct vb2_v4l2_buffer *src_buf,
			    struct vb2_v4l2_buffer *dst_buf,
			    enum vb2_buffer_state result)
{
	struct v4l2_ctrl_vp9_frame_decode_params *dec_params;
	struct rkvdec_decoded_buffer *dec_dst_buf;
	const struct v4l2_ctrl_vp9_frame_ctx *fctx;
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct v4l2_ctrl *ctrl;
	unsigned int fctx_idx;

	if (result == VB2_BUF_STATE_ERROR)
		return;

	dec_dst_buf = vb2_to_rkvdec_decoded_buf(&dst_buf->vb2_buf);
	dec_params = &dec_dst_buf->vp9.params;
	fctx_idx = dec_params->frame_context_idx;

	fctx = ctrl->p_cur.p;

	if (!(dec_params->flags &
	      (V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT |
	       V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE)))
		adapt_probs(ctx, dec_dst_buf, vp9_ctx->count_tbl.cpu);

	if (!(dec_params->flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX))
		return;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_MPEG_VIDEO_VP9_FRAME_CONTEXT(fctx_idx));
	if (WARN_ON(!ctrl))
		return;

	v4l2_ctrl_s_ctrl_compound(ctrl, &dec_dst_buf->vp9.params.probs,
				  sizeof(dec_dst_buf->vp9.params.probs));
}

static int rkvdec_vp9_start(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_vp9_priv_tbl *priv_tbl;
	struct rkvdec_vp9_ctx *vp9_ctx;
	u8 *count_tbl;
	int ret;

	vp9_ctx = kzalloc(sizeof(*vp9_ctx), GFP_KERNEL);
	if (!vp9_ctx)
		return -ENOMEM;

	ctx->priv = vp9_ctx;

	priv_tbl = dma_alloc_coherent(rkvdec->dev, sizeof(*priv_tbl),
				      &vp9_ctx->priv_tbl.dma, GFP_KERNEL);
	if (!priv_tbl) {
		ret = -ENOMEM;
		goto err_free_ctx;
	}

	vp9_ctx->priv_tbl.size = sizeof(*priv_tbl);
	vp9_ctx->priv_tbl.cpu = priv_tbl;
	memset(priv_tbl, 0, sizeof(*priv_tbl));

	count_tbl = dma_alloc_coherent(rkvdec->dev, RKVDEC_VP9_COUNT_SIZE,
				       &vp9_ctx->count_tbl.dma, GFP_KERNEL);
	if (!count_tbl) {
		ret = -ENOMEM;
		goto err_free_priv_tbl;
	}

	vp9_ctx->count_tbl.size = RKVDEC_VP9_COUNT_SIZE;
	vp9_ctx->count_tbl.cpu = count_tbl;
	memset(count_tbl, 0, sizeof(*count_tbl));

	return 0;

err_free_priv_tbl:
	dma_free_coherent(rkvdec->dev, vp9_ctx->priv_tbl.size,
			  vp9_ctx->priv_tbl.cpu, vp9_ctx->priv_tbl.dma);

err_free_ctx:
	kfree(ctx);
	return ret;
}

static void rkvdec_vp9_stop(struct rkvdec_ctx *ctx)
{
	struct rkvdec_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_dev *rkvdec = ctx->dev;

	dma_free_coherent(rkvdec->dev, vp9_ctx->count_tbl.size,
			  vp9_ctx->count_tbl.cpu, vp9_ctx->count_tbl.dma);
	dma_free_coherent(rkvdec->dev, vp9_ctx->priv_tbl.size,
			  vp9_ctx->priv_tbl.cpu, vp9_ctx->priv_tbl.dma);
	kfree(vp9_ctx);
}

static int rkvdec_vp9_adjust_fmt(struct rkvdec_ctx *ctx,
				 struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *fmt = &f->fmt.pix_mp;

	fmt->num_planes = 1;
	fmt->plane_fmt[0].sizeimage = fmt->width * fmt->height * 2;
	return 0;
}

const struct rkvdec_coded_fmt_ops rkvdec_vp9_fmt_ops = {
	.adjust_fmt = rkvdec_vp9_adjust_fmt,
	.start = rkvdec_vp9_start,
	.stop = rkvdec_vp9_stop,
	.run = rkvdec_vp9_run,
	.done = rkvdec_vp9_done,
};
