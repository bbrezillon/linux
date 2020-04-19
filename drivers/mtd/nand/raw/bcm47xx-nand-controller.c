// SPDX-License-Identifier: GPL-2.0-only
/*
 * BCM47XX NAND flash driver
 *
 * Copyright (C) 2012 Rafał Miłecki <zajec5@gmail.com>
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/bcma/bcma.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>

/* Broadcom uses 1'000'000 but it seems to be too many. Tests on WNDR4500 has
 * shown ~1000 retries as maxiumum. */
#define NFLASH_READY_RETRIES		10000

#define NFLASH_SECTOR_SIZE		512

#define NCTL_CMD0			0x00010000
#define NCTL_COL			0x00020000	/* Update column with value from BCMA_CC_NFLASH_COL_ADDR */
#define NCTL_ROW			0x00040000	/* Update row (page) with value from BCMA_CC_NFLASH_ROW_ADDR */
#define NCTL_CMD1W			0x00080000
#define NCTL_READ			0x00100000
#define NCTL_WRITE			0x00200000
/* When the SPECADDR is set CMD1 is interpreted as a single ADDR cycle */
#define NCTL_SPECADDR			0x01000000
#define NCTL_READY			0x04000000
#define NCTL_ERR			0x08000000
/*
 * Number of DATA cycles to issue when NCTL_{READ,WRITE} is set. The minimum
 * value is 1 and the maximum value is 4. Those bytes are then stored in the
 * BCMA_CC_NFLASH_DATA register.
 */
#define NCTL_DATA_CYCLES(x)		((((x) - 1) & 0x3) << 28)
/*
 * The CS pin seems to be asserted even if NCTL_CSA is not set. All this bit
 * seems to encode is whether the CS line should stay asserted after the
 * operation has been executed. In other words, you should only set it if you
 * intend to do more operations on the NAND bus.
 */
#define NCTL_CSA			0x40000000
#define NCTL_START			0x80000000

#define CONF_MAGIC_BIT			0x00000002
#define CONF_COL_BYTES(x)		(((x) - 1) << 4)
#define CONF_ROW_BYTES(x)		(((x) - 1) << 6)

#ifndef pr_fmt
#define pr_fmt(fmt)		KBUILD_MODNAME ": " fmt
#endif

struct bcm47xxnflash {
	struct nand_controller base;
	struct bcma_drv_cc *cc;

	struct nand_chip nand_chip;
};

/**************************************************
 * Various helpers
 **************************************************/

static inline u8 bcm47xxnflash_ops_bcm4706_ns_to_cycle(u16 ns, u16 clock)
{
	return ((ns * 1000 * clock) / 1000000) + 1;
}

static int bcm47xxnflash_ops_bcm4706_ctl_cmd(struct bcma_drv_cc *cc, u32 code)
{
	int i = 0;

	bcma_cc_write32(cc, BCMA_CC_NFLASH_CTL, NCTL_START | code);
	for (i = 0; i < NFLASH_READY_RETRIES; i++) {
		if (!(bcma_cc_read32(cc, BCMA_CC_NFLASH_CTL) & NCTL_START)) {
			i = 0;
			break;
		}
	}
	if (i) {
		pr_err("NFLASH control command not ready!\n");
		return -EBUSY;
	}
	return 0;
}

/**************************************************
 * NAND chip ops
 **************************************************/

static int
bcm47xxnflash_ops_bcm4706_exec_cmd_addr(struct nand_chip *chip,
					const struct nand_subop *subop)
{
	struct bcm47xxnflash *b47n = nand_get_controller_data(chip);
	u32 nctl = 0, col = 0, row = 0, ncols = 0, nrows = 0;
	unsigned int i, j;

	for (i = 0; i < subop->ninstrs; i++) {
		const struct nand_op_instr *instr = &subop->instrs[i];

		switch (instr->type) {
		case NAND_OP_CMD_INSTR:
			if (WARN_ON_ONCE((nctl & NCTL_CMD0) &&
					 (nctl & NCTL_CMD1W)))
				return -EINVAL;
			else if (nctl & NCTL_CMD0)
				nctl |= NCTL_CMD1W |
					((u32)instr->ctx.cmd.opcode << 8);
			else
				nctl |= NCTL_CMD0 | instr->ctx.cmd.opcode;
			break;
		case NAND_OP_ADDR_INSTR:
			for (j = 0; j < instr->ctx.addr.naddrs; j++) {
				u32 addr = instr->ctx.addr.addrs[j];

				if (j < 2) {
					col |= addr << (j * 8);
					nctl |= NCTL_COL;
					ncols++;
				} else {
					row |= addr << ((j - 2) * 8);
					nctl |= NCTL_ROW;
					nrows++;
				}
			}
			break;
		default:
			return -EINVAL;
		}
	}

	/* Keep the CS line asserted if there's something else to execute. */
	if (!subop->is_last)
		nctl |= NCTL_CSA;

	bcma_cc_write32(b47n->cc, BCMA_CC_NFLASH_CONF,
			CONF_MAGIC_BIT |
			CONF_COL_BYTES(ncols) |
			CONF_ROW_BYTES(nrows));
	return bcm47xxnflash_ops_bcm4706_ctl_cmd(b47n->cc, nctl);
}

static int
bcm47xxnflash_ops_bcm4706_exec_waitrdy(struct nand_chip *chip,
				       const struct nand_subop *subop)
{
	struct bcm47xxnflash *b47n = nand_get_controller_data(chip);
	const struct nand_op_instr *instr = &subop->instrs[0];

	return nand_poll(bcma_cc_read32(b47n->cc, BCMA_CC_NFLASH_CTL) & NCTL_READY,
			 10, 100, instr->ctx.waitrdy.timeout_ms, false);
}

static int
bcm47xxnflash_ops_bcm4706_exec_rw(struct nand_chip *chip,
				  const struct nand_subop *subop)
{
	struct bcm47xxnflash *b47n = nand_get_controller_data(chip);
	const struct nand_op_instr *instr = &subop->instrs[0];
	unsigned int i;
	int ret;

	for (i = 0; i < instr->ctx.data.len; i += 4) {
		unsigned int nbytes = min_t(unsigned int,
					    instr->ctx.data.len - i, 4);
		u32 nctl, data;

		nctl = NCTL_CSA | NCTL_DATA_CYCLES(nbytes);
		if (instr->type == NAND_OP_DATA_IN_INSTR) {
			nctl |= NCTL_READ;
		} else {
			nctl |= NCTL_WRITE;
			memcpy(&data, instr->ctx.data.buf.in + i, nbytes);
			bcma_cc_write32(b47n->cc, BCMA_CC_NFLASH_DATA, data);
		}

		if (i + nbytes < instr->ctx.data.len)
			nctl |= NCTL_CSA;

		ret = bcm47xxnflash_ops_bcm4706_ctl_cmd(b47n->cc, nctl);
		if (ret)
			return ret;

		if (instr->type == NAND_OP_DATA_IN_INSTR) {
			data = bcma_cc_read32(b47n->cc, BCMA_CC_NFLASH_DATA);
			memcpy(instr->ctx.data.buf.in + i, &data, nbytes);
		}
	}

	return 0;
}

static const struct nand_op_parser bcm47xxnflash_op_parser = NAND_OP_PARSER(
	NAND_OP_PARSER_PATTERN(bcm47xxnflash_ops_bcm4706_exec_cmd_addr,
			       NAND_OP_PARSER_PAT_CMD_ELEM(true),
			       NAND_OP_PARSER_PAT_ADDR_ELEM(true, 5),
			       NAND_OP_PARSER_PAT_CMD_ELEM(true)),
	NAND_OP_PARSER_PATTERN(bcm47xxnflash_ops_bcm4706_exec_waitrdy,
			       NAND_OP_PARSER_PAT_WAITRDY_ELEM(false)),
	NAND_OP_PARSER_PATTERN(bcm47xxnflash_ops_bcm4706_exec_rw,
			       NAND_OP_PARSER_PAT_DATA_IN_ELEM(false, 0x200)),
	NAND_OP_PARSER_PATTERN(bcm47xxnflash_ops_bcm4706_exec_rw,
			       NAND_OP_PARSER_PAT_DATA_OUT_ELEM(false, 0x200)),
);

static int
bcm47xxnflash_ops_bcm4706_exec_op(struct nand_chip *chip,
				  const struct nand_operation *op,
				  bool check_only)
{
	return nand_op_parser_exec_op(chip, &bcm47xxnflash_op_parser, op,
				      check_only);
}

static const struct nand_controller_ops bcm47xxnflash_ops = {
	.exec_op = bcm47xxnflash_ops_bcm4706_exec_op,
};

/**************************************************
 * Init
 **************************************************/

static int bcm47xxnflash_ops_bcm4706_init(struct bcm47xxnflash *b47n)
{
	int err;
	u32 freq;
	u16 clock;
	u8 w0, w1, w2, w3, w4;

	nand_controller_init(&b47n->base);
	b47n->base.ops = &bcm47xxnflash_ops;
	b47n->nand_chip.controller = &b47n->base;
	b47n->nand_chip.bbt_options = NAND_BBT_USE_FLASH;
	b47n->nand_chip.ecc.mode = NAND_ECC_NONE; /* TODO: implement ECC */

	/* Enable NAND flash access */
	bcma_cc_set32(b47n->cc, BCMA_CC_4706_FLASHSCFG,
		      BCMA_CC_4706_FLASHSCFG_NF1);

	/* Configure wait counters */
	if (b47n->cc->status & BCMA_CC_CHIPST_4706_PKG_OPTION) {
		/* 400 MHz */
		freq = 400000000 / 4;
	} else {
		freq = bcma_chipco_pll_read(b47n->cc, 4);
		freq = (freq & 0xFFF) >> 3;
		/* Fixed reference clock 25 MHz and m = 2 */
		freq = (freq * 25000000 / 2) / 4;
	}
	clock = freq / 1000000;
	w0 = bcm47xxnflash_ops_bcm4706_ns_to_cycle(15, clock);
	w1 = bcm47xxnflash_ops_bcm4706_ns_to_cycle(20, clock);
	w2 = bcm47xxnflash_ops_bcm4706_ns_to_cycle(10, clock);
	w3 = bcm47xxnflash_ops_bcm4706_ns_to_cycle(10, clock);
	w4 = bcm47xxnflash_ops_bcm4706_ns_to_cycle(100, clock);
	bcma_cc_write32(b47n->cc, BCMA_CC_NFLASH_WAITCNT0,
			(w4 << 24 | w3 << 18 | w2 << 12 | w1 << 6 | w0));

	/* Scan NAND */
	err = nand_scan(&b47n->nand_chip, 1);
	if (err) {
		pr_err("Could not scan NAND flash: %d\n", err);
		bcma_cc_mask32(b47n->cc, BCMA_CC_4706_FLASHSCFG,
			       ~BCMA_CC_4706_FLASHSCFG_NF1);
	}

	return err;
}

static const char *probes[] = { "bcm47xxpart", NULL };

static int bcm47xxnflash_probe(struct platform_device *pdev)
{
	struct bcma_nflash *nflash = dev_get_platdata(&pdev->dev);
	struct bcm47xxnflash *b47n;
	struct mtd_info *mtd;
	int err = 0;

	b47n = devm_kzalloc(&pdev->dev, sizeof(*b47n), GFP_KERNEL);
	if (!b47n)
		return -ENOMEM;

	nand_set_controller_data(&b47n->nand_chip, b47n);
	mtd = nand_to_mtd(&b47n->nand_chip);
	mtd->dev.parent = &pdev->dev;
	b47n->cc = container_of(nflash, struct bcma_drv_cc, nflash);

	if (b47n->cc->core->bus->chipinfo.id == BCMA_CHIP_ID_BCM4706) {
		err = bcm47xxnflash_ops_bcm4706_init(b47n);
	} else {
		pr_err("Device not supported\n");
		err = -ENOTSUPP;
	}
	if (err) {
		pr_err("Initialization failed: %d\n", err);
		return err;
	}

	platform_set_drvdata(pdev, b47n);

	err = mtd_device_parse_register(mtd, probes, NULL, NULL, 0);
	if (err) {
		pr_err("Failed to register MTD device: %d\n", err);
		return err;
	}

	return 0;
}

static int bcm47xxnflash_remove(struct platform_device *pdev)
{
	struct bcm47xxnflash *nflash = platform_get_drvdata(pdev);
	struct nand_chip *chip = &nflash->nand_chip;
	int ret;

	ret = mtd_device_unregister(nand_to_mtd(chip));
	WARN_ON(ret);
	nand_cleanup(chip);

	return 0;
}

static struct platform_driver bcm47xxnflash_driver = {
	.probe	= bcm47xxnflash_probe,
	.remove = bcm47xxnflash_remove,
	.driver = {
		.name = "bcma_nflash",
	},
};
module_platform_driver(bcm47xxnflash_driver);

MODULE_DESCRIPTION("NAND flash driver for BCMA bus");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafał Miłecki");
