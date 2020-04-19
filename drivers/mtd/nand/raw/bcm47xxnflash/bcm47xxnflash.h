/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BCM47XXNFLASH_H
#define __BCM47XXNFLASH_H

#ifndef pr_fmt
#define pr_fmt(fmt)		KBUILD_MODNAME ": " fmt
#endif

#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>

struct bcm47xxnflash {
	struct nand_controller base;
	struct bcma_drv_cc *cc;

	struct nand_chip nand_chip;
};

int bcm47xxnflash_ops_bcm4706_init(struct bcm47xxnflash *b47n);

#endif /* BCM47XXNFLASH */
