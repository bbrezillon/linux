// SPDX-License-Identifier: GPL-2.0-only
/*
 * Oxford Semiconductor OXNAS NAND driver

 * Copyright (C) 2016 Neil Armstrong <narmstrong@baylibre.com>
 * Heavily based on plat_nand.c :
 * Author: Vitaly Wool <vitalywool@gmail.com>
 * Copyright (C) 2013 Ma Haijun <mahaijuns@gmail.com>
 * Copyright (C) 2012 John Crispin <blogic@openwrt.org>
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>
#include <linux/of.h>

/* Nand commands */
#define OXNAS_NAND_CMD_ALE		BIT(18)
#define OXNAS_NAND_CMD_CLE		BIT(19)

#define OXNAS_NAND_MAX_CHIPS	1

struct oxnas_nand_ctrl {
	struct nand_controller base;
	void __iomem *io_base;
	struct clk *clk;
	struct nand_chip *chips[OXNAS_NAND_MAX_CHIPS];
	unsigned int nchips;
};

static int oxnas_nand_exec_instr(struct nand_chip *chip,
				const struct nand_op_instr *instr)
{
	struct oxnas_nand_ctrl *oxnas = nand_get_controller_data(chip);
	unsigned int i;

	switch (instr->type) {
	case NAND_OP_CMD_INSTR:
		writeb(instr->ctx.cmd.opcode, oxnas->io_base + OXNAS_NAND_CMD_CLE);
		return 0;

	case NAND_OP_ADDR_INSTR:
		for (i = 0; i < instr->ctx.addr.naddrs; i++)
			writeb(instr->ctx.addr.addrs[i],
			       oxnas->io_base + OXNAS_NAND_CMD_ALE);
		return 0;

	case NAND_OP_DATA_IN_INSTR:
		ioread8_rep(oxnas->io_base, instr->ctx.data.buf.in, instr->ctx.data.len);
		return 0;

	case NAND_OP_DATA_OUT_INSTR:
		iowrite8_rep(oxnas->io_base, instr->ctx.data.buf.out, instr->ctx.data.len);
		return 0;

	case NAND_OP_WAITRDY_INSTR:
		return nand_soft_waitrdy(chip, instr->ctx.waitrdy.timeout_ms);

	default:
		return -EINVAL;
	}

	return 0;
}

static int oxnas_nand_exec_op(struct nand_chip *chip,
			     const struct nand_operation *op,
			     bool check_only)
{
	unsigned int i;
	int ret = 0;

	if (check_only)
		return 0;

	for (i = 0; i < op->ninstrs; i++) {
		ret = oxnas_nand_exec_instr(chip, &op->instrs[i]);
		if (ret)
			break;

		if (op->instrs[i].delay_ns)
			ndelay(op->instrs[i].delay_ns);
	}

	return ret;
}

static const struct nand_controller_ops oxnas_nand_ops = {
	.exec_op = oxnas_nand_exec_op,
};

/*
 * Probe for the NAND device.
 */
static int oxnas_nand_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *nand_np;
	struct oxnas_nand_ctrl *oxnas;
	struct nand_chip *chip;
	struct mtd_info *mtd;
	struct resource *res;
	int count = 0;
	int err = 0;
	int i;

	/* Allocate memory for the device structure (and zero it) */
	oxnas = devm_kzalloc(&pdev->dev, sizeof(*oxnas),
			     GFP_KERNEL);
	if (!oxnas)
		return -ENOMEM;

	nand_controller_init(&oxnas->base);
	oxnas->base.ops = &oxnas_nand_ops;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	oxnas->io_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(oxnas->io_base))
		return PTR_ERR(oxnas->io_base);

	oxnas->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(oxnas->clk))
		oxnas->clk = NULL;

	/* Only a single chip node is supported */
	count = of_get_child_count(np);
	if (count > 1)
		return -EINVAL;

	err = clk_prepare_enable(oxnas->clk);
	if (err)
		return err;

	device_reset_optional(&pdev->dev);

	for_each_child_of_node(np, nand_np) {
		chip = devm_kzalloc(&pdev->dev, sizeof(struct nand_chip),
				    GFP_KERNEL);
		if (!chip) {
			err = -ENOMEM;
			goto err_release_child;
		}

		chip->controller = &oxnas->base;

		nand_set_flash_node(chip, nand_np);
		nand_set_controller_data(chip, oxnas);

		mtd = nand_to_mtd(chip);
		mtd->dev.parent = &pdev->dev;
		mtd->priv = chip;

		/* Scan to find existence of the device */
		err = nand_scan(chip, 1);
		if (err)
			goto err_release_child;

		err = mtd_device_register(mtd, NULL, 0);
		if (err)
			goto err_cleanup_nand;

		oxnas->chips[oxnas->nchips] = chip;
		++oxnas->nchips;
	}

	/* Exit if no chips found */
	if (!oxnas->nchips) {
		err = -ENODEV;
		goto err_clk_unprepare;
	}

	platform_set_drvdata(pdev, oxnas);

	return 0;

err_cleanup_nand:
	nand_cleanup(chip);
err_release_child:
	of_node_put(nand_np);

	for (i = 0; i < oxnas->nchips; i++) {
		chip = oxnas->chips[i];
		WARN_ON(mtd_device_unregister(nand_to_mtd(chip)));
		nand_cleanup(chip);
	}

err_clk_unprepare:
	clk_disable_unprepare(oxnas->clk);
	return err;
}

static int oxnas_nand_remove(struct platform_device *pdev)
{
	struct oxnas_nand_ctrl *oxnas = platform_get_drvdata(pdev);
	struct nand_chip *chip;
	int i;

	for (i = 0; i < oxnas->nchips; i++) {
		chip = oxnas->chips[i];
		WARN_ON(mtd_device_unregister(nand_to_mtd(chip)));
		nand_cleanup(chip);
	}

	clk_disable_unprepare(oxnas->clk);

	return 0;
}

static const struct of_device_id oxnas_nand_match[] = {
	{ .compatible = "oxsemi,ox820-nand" },
	{},
};
MODULE_DEVICE_TABLE(of, oxnas_nand_match);

static struct platform_driver oxnas_nand_driver = {
	.probe	= oxnas_nand_probe,
	.remove	= oxnas_nand_remove,
	.driver	= {
		.name		= "oxnas_nand",
		.of_match_table = oxnas_nand_match,
	},
};

module_platform_driver(oxnas_nand_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Oxnas NAND driver");
MODULE_ALIAS("platform:oxnas_nand");
