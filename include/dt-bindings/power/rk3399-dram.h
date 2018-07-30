/* SPDX-License-Identifier: (GPL-2.0+ OR X11) */
/*
 * Copyright (c) 2016-2018, Fuzhou Rockchip Electronics Co., Ltd
 *
 * Author: Lin Huang <hl@rock-chips.com>
 */

#ifndef _DTS_DRAM_ROCKCHIP_RK3399_H
#define _DTS_DRAM_ROCKCHIP_RK3399_H

#define DDR3_DS_34ohm		34
#define DDR3_DS_40ohm		40

#define DDR3_ODT_DIS		0
#define DDR3_ODT_40ohm		40
#define DDR3_ODT_60ohm		60
#define DDR3_ODT_120ohm		120

#define LP2_DS_34ohm		34
#define LP2_DS_40ohm		40
#define LP2_DS_48ohm		48
#define LP2_DS_60ohm		60
#define LP2_DS_68_6ohm		68	/* optional */
#define LP2_DS_80ohm		80
#define LP2_DS_120ohm		120	/* optional */

#define LP3_DS_34ohm		34
#define LP3_DS_40ohm		40
#define LP3_DS_48ohm		48
#define LP3_DS_60ohm		60
#define LP3_DS_80ohm		80
#define LP3_DS_34D_40U		3440
#define LP3_DS_40D_48U		4048
#define LP3_DS_34D_48U		3448

#define LP3_ODT_DIS		0
#define LP3_ODT_60ohm		60
#define LP3_ODT_120ohm		120
#define LP3_ODT_240ohm		240

#define LP4_PDDS_40ohm		40
#define LP4_PDDS_48ohm		48
#define LP4_PDDS_60ohm		60
#define LP4_PDDS_80ohm		80
#define LP4_PDDS_120ohm		120
#define LP4_PDDS_240ohm		240

#define LP4_DQ_ODT_40ohm	40
#define LP4_DQ_ODT_48ohm	48
#define LP4_DQ_ODT_60ohm	60
#define LP4_DQ_ODT_80ohm	80
#define LP4_DQ_ODT_120ohm	120
#define LP4_DQ_ODT_240ohm	240
#define LP4_DQ_ODT_DIS		0

#define LP4_CA_ODT_40ohm	40
#define LP4_CA_ODT_48ohm	48
#define LP4_CA_ODT_60ohm	60
#define LP4_CA_ODT_80ohm	80
#define LP4_CA_ODT_120ohm	120
#define LP4_CA_ODT_240ohm	240
#define LP4_CA_ODT_DIS		0

#define PHY_DRV_ODT_Hi_Z	0
#define PHY_DRV_ODT_240		240
#define PHY_DRV_ODT_120		120
#define PHY_DRV_ODT_80		80
#define PHY_DRV_ODT_60		60
#define PHY_DRV_ODT_48		48
#define PHY_DRV_ODT_40		40
#define PHY_DRV_ODT_34_3	34

#endif /* _DTS_DRAM_ROCKCHIP_RK3399_H */
