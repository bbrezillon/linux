// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Overview:
 *   Platform independent driver for NDFC (NanD Flash Controller)
 *   integrated into EP440 cores
 *
 *   Ported to an OF platform driver by Sean MacLennan
 *
 *   The NDFC supports multiple chips, but this driver only supports a
 *   single chip since I do not have access to any boards with
 *   multiple chips.
 *
 *  Author: Thomas Gleixner
 *
 *  Copyright 2006 IBM
 *  Copyright 2008 PIKA Technologies
 *    Sean MacLennan <smaclennan@pikatech.com>
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/nand_ecc.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/ndfc.h>
#include <linux/slab.h>
#include <linux/mtd/mtd.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <asm/io.h>

#define NDFC_MAX_CS    4

struct ndfc_controller {
	struct platform_device *ofdev;
	void __iomem *ndfcbase;
	struct nand_chip chip;
	int chip_select;
	struct nand_controller ndfc_control;
};

static struct ndfc_controller ndfc_ctrl[NDFC_MAX_CS];

static void ndfc_enable_hwecc(struct nand_chip *chip, int mode)
{
	uint32_t ccr;
	struct ndfc_controller *ndfc = nand_get_controller_data(chip);

	ccr = in_be32(ndfc->ndfcbase + NDFC_CCR);
	ccr |= NDFC_CCR_RESET_ECC;
	out_be32(ndfc->ndfcbase + NDFC_CCR, ccr);
	wmb();
}

static int ndfc_calculate_ecc(struct nand_chip *chip,
			      const u_char *dat, u_char *ecc_code)
{
	struct ndfc_controller *ndfc = nand_get_controller_data(chip);
	uint32_t ecc;
	uint8_t *p = (uint8_t *)&ecc;

	wmb();
	ecc = in_be32(ndfc->ndfcbase + NDFC_ECC);
	/* The NDFC uses Smart Media (SMC) bytes order */
	ecc_code[0] = p[1];
	ecc_code[1] = p[2];
	ecc_code[2] = p[3];

	return 0;
}

/*
 * Initialize chip structure
 */
static int ndfc_chip_init(struct ndfc_controller *ndfc,
			  struct device_node *node)
{
	struct device_node *flash_np;
	struct nand_chip *chip = &ndfc->chip;
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret;

	chip->controller = &ndfc->ndfc_control;
	chip->ecc.correct = nand_correct_data;
	chip->ecc.hwctl = ndfc_enable_hwecc;
	chip->ecc.calculate = ndfc_calculate_ecc;
	chip->ecc.mode = NAND_ECC_HW;
	chip->ecc.size = 256;
	chip->ecc.bytes = 3;
	chip->ecc.strength = 1;
	nand_set_controller_data(chip, ndfc);

	mtd->dev.parent = &ndfc->ofdev->dev;

	flash_np = of_get_next_child(node, NULL);
	if (!flash_np)
		return -ENODEV;
	nand_set_flash_node(chip, flash_np);

	mtd->name = kasprintf(GFP_KERNEL, "%s.%pOFn", dev_name(&ndfc->ofdev->dev),
			      flash_np);
	if (!mtd->name) {
		ret = -ENOMEM;
		goto err;
	}

	ret = nand_scan(chip, 1);
	if (ret)
		goto err;

	ret = mtd_device_register(mtd, NULL, 0);

err:
	of_node_put(flash_np);
	if (ret)
		kfree(mtd->name);
	return ret;
}

static void ndfc_data_in(struct nand_chip *chip,
			 const struct nand_op_instr *instr)
{
	struct ndfc_controller *ndfc = nand_get_controller_data(chip);
	unsigned int i;

	if (!instr->ctx.data.force_8bit &&
	    !(instr->ctx.data.len & 3) &&
	    !((uintptr_t)instr->ctx.data.buf.in & 3)) {
		u32 *in = instr->ctx.data.buf.in;

		for (i = 0; i < instr->ctx.data.len / 4; i++)
			in[i] = in_be32(ndfc->ndfcbase + NDFC_DATA);
	} else {
		ioread8_rep(ndfc->ndfcbase + NDFC_DATA,
			    instr->ctx.data.buf.in,
			    instr->ctx.data.len);
	}
}

static void ndfc_data_out(struct nand_chip *chip,
			  const struct nand_op_instr *instr)
{
	struct ndfc_controller *ndfc = nand_get_controller_data(chip);
	unsigned int i;

	if (!instr->ctx.data.force_8bit &&
	    !(instr->ctx.data.len & 3) &&
	    !((uintptr_t)instr->ctx.data.buf.out & 3)) {
		const u32 *out = instr->ctx.data.buf.out;

		for (i = 0; i < instr->ctx.data.len / 4; i++)
			out_be32(ndfc->ndfcbase + NDFC_DATA, out[i]);
	} else {
		iowrite8_rep(ndfc->ndfcbase + NDFC_DATA,
			     instr->ctx.data.buf.out,
			     instr->ctx.data.len);
	}
}

static int ndfc_exec_instr(struct nand_chip *chip,
			   const struct nand_op_instr *instr)
{
	struct ndfc_controller *ndfc = nand_get_controller_data(chip);
	unsigned int i;

	switch (instr->type) {
	case NAND_OP_CMD_INSTR:
		writel(instr->ctx.cmd.opcode, ndfc->ndfcbase + NDFC_CMD);
		return 0;

	case NAND_OP_ADDR_INSTR:
		for (i = 0; i < instr->ctx.addr.naddrs; i++)
			writel(instr->ctx.addr.addrs[i], ndfc->ndfcbase + NDFC_ALE);
		return 0;

	case NAND_OP_DATA_IN_INSTR:
		ndfc_data_in(chip, instr);
		return 0;

	case NAND_OP_DATA_OUT_INSTR:
		ndfc_data_out(chip, instr);
		return 0;

	case NAND_OP_WAITRDY_INSTR:
		return nand_poll(in_be32(ndfc->ndfcbase + NDFC_STAT) & NDFC_STAT_IS_READY,
				 10, 10, instr->ctx.waitrdy.timeout_ms, true);

	default:
		return -EINVAL;
	}

	return 0;
}

static int ndfc_exec_op(struct nand_chip *chip,
			const struct nand_operation *op,
			bool check_only)
{
	struct ndfc_controller *ndfc = nand_get_controller_data(chip);
	unsigned int i;
	int ret = 0;
	u32 ccr;

	if (check_only)
		return 0;

	ccr = in_be32(ndfc->ndfcbase + NDFC_CCR) & ~NDFC_CCR_BS_MASK;
	out_be32(ndfc->ndfcbase + NDFC_CCR,
	         ccr | NDFC_CCR_BS(op->cs + ndfc->chip_select));
	for (i = 0; i < op->ninstrs; i++) {
		ret = ndfc_exec_instr(chip, &op->instrs[i]);
		if (ret)
			break;

		if (op->instrs[i].delay_ns)
			ndelay(op->instrs[i].delay_ns);
	}
	out_be32(ndfc->ndfcbase + NDFC_CCR, ccr | NDFC_CCR_RESET_CE);

	return ret;
}

static const struct nand_controller_ops ndfc_ops = {
	.exec_op = ndfc_exec_op,
};

static int ndfc_probe(struct platform_device *ofdev)
{
	struct ndfc_controller *ndfc;
	const __be32 *reg;
	u32 ccr;
	u32 cs;
	int err, len;

	/* Read the reg property to get the chip select */
	reg = of_get_property(ofdev->dev.of_node, "reg", &len);
	if (reg == NULL || len != 12) {
		dev_err(&ofdev->dev, "unable read reg property (%d)\n", len);
		return -ENOENT;
	}

	cs = be32_to_cpu(reg[0]);
	if (cs >= NDFC_MAX_CS) {
		dev_err(&ofdev->dev, "invalid CS number (%d)\n", cs);
		return -EINVAL;
	}

	ndfc = &ndfc_ctrl[cs];
	ndfc->chip_select = cs;

	nand_controller_init(&ndfc->ndfc_control);
	ndfc->ndfc_control.ops = &ndfc_ops;
	ndfc->ofdev = ofdev;
	dev_set_drvdata(&ofdev->dev, ndfc);

	ndfc->ndfcbase = of_iomap(ofdev->dev.of_node, 0);
	if (!ndfc->ndfcbase) {
		dev_err(&ofdev->dev, "failed to get memory\n");
		return -EIO;
	}

	ccr = NDFC_CCR_BS(ndfc->chip_select);

	/* It is ok if ccr does not exist - just default to 0 */
	reg = of_get_property(ofdev->dev.of_node, "ccr", NULL);
	if (reg)
		ccr |= be32_to_cpup(reg);

	out_be32(ndfc->ndfcbase + NDFC_CCR, ccr);

	/* Set the bank settings if given */
	reg = of_get_property(ofdev->dev.of_node, "bank-settings", NULL);
	if (reg) {
		int offset = NDFC_BCFG0 + (ndfc->chip_select << 2);
		out_be32(ndfc->ndfcbase + offset, be32_to_cpup(reg));
	}

	err = ndfc_chip_init(ndfc, ofdev->dev.of_node);
	if (err) {
		iounmap(ndfc->ndfcbase);
		return err;
	}

	return 0;
}

static int ndfc_remove(struct platform_device *ofdev)
{
	struct ndfc_controller *ndfc = dev_get_drvdata(&ofdev->dev);
	struct nand_chip *chip = &ndfc->chip;
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret;

	ret = mtd_device_unregister(mtd);
	WARN_ON(ret);
	nand_cleanup(chip);
	kfree(mtd->name);

	return 0;
}

static const struct of_device_id ndfc_match[] = {
	{ .compatible = "ibm,ndfc", },
	{}
};
MODULE_DEVICE_TABLE(of, ndfc_match);

static struct platform_driver ndfc_driver = {
	.driver = {
		.name = "ndfc",
		.of_match_table = ndfc_match,
	},
	.probe = ndfc_probe,
	.remove = ndfc_remove,
};

module_platform_driver(ndfc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thomas Gleixner <tglx@linutronix.de>");
MODULE_DESCRIPTION("OF Platform driver for NDFC");
