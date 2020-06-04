// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2006-2007 PA Semi, Inc
 *
 * Author: Egor Martovetsky <egor@pasemi.com>
 * Maintained by: Olof Johansson <olof@lixom.net>
 *
 * Driver for the PWRficient onchip NAND flash interface
 */

#undef DEBUG

#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/nand_ecc.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pci.h>

#include <asm/io.h>

#define LBICTRL_LPCCTL_NR		0x00004000
#define CLE_PIN_CTL			15
#define ALE_PIN_CTL			14

struct pasemi_nand_controller {
	struct nand_controller base;
	struct nand_chip chip;
	void __iomem *io;
};

static struct pasemi_nand_controller *chip_to_pasemi(struct nand_chip *chip)
{
	return container_of(chip->controller, struct pasemi_nand_controller, base);
}

static unsigned int lpcctl;
static struct mtd_info *pasemi_nand_mtd;
static const char driver_name[] = "pasemi-nand";

static void pasemi_read_buf(struct nand_chip *chip, u_char *buf, int len)
{
	struct pasemi_nand_controller *pasemi = chip_to_pasemi(chip);

	while (len > 0x800) {
		memcpy_fromio(buf, pasemi->io, 0x800);
		buf += 0x800;
		len -= 0x800;
	}
	memcpy_fromio(buf, pasemi->io, len);
}

static void pasemi_write_buf(struct nand_chip *chip, const u_char *buf,
			     int len)
{
	struct pasemi_nand_controller *pasemi = chip_to_pasemi(chip);

	while (len > 0x800) {
		memcpy_toio(pasemi->io, buf, 0x800);
		buf += 0x800;
		len -= 0x800;
	}
	memcpy_toio(pasemi->io, buf, len);
}

static void pasemi_hwcontrol(struct nand_chip *chip, int cmd,
			     unsigned int ctrl)
{
	if (cmd == NAND_CMD_NONE)
		return;

	if (ctrl & NAND_CLE)
		out_8(chip->legacy.IO_ADDR_W + (1 << CLE_PIN_CTL), cmd);
	else
		out_8(chip->legacy.IO_ADDR_W + (1 << ALE_PIN_CTL), cmd);

	/* Push out posted writes */
	eieio();
	inl(lpcctl);
}

int pasemi_device_ready(struct nand_chip *chip)
{
	return !!(inl(lpcctl) & LBICTRL_LPCCTL_NR);
}

static int pasemi_exec_instr(struct nand_chip *chip,
			     const struct nand_op_instr *instr)
{
	struct pasemi_nand_controller *pasemi = chip_to_pasemi(chip);
	unsigned int i;

	switch (instr->type) {
	case NAND_OP_CMD_INSTR:
		out_8(pasemi->io + (1 << CLE_PIN_CTL), instr->ctx.cmd.opcode);

		/* Push out posted writes */
		eieio();
		inl(lpcctl);
		return 0;

	case NAND_OP_ADDR_INSTR:
		for (i = 0; i < instr->ctx.addr.naddrs; i++)
			out_8(pasemi->io + (1 << ALE_PIN_CTL),
			      instr->ctx.addr.addrs[i]);

		/* Push out posted writes */
		eieio();
		inl(lpcctl);
		return 0;

	case NAND_OP_DATA_IN_INSTR:
		if (instr->ctx.data.force_8bit)
			ioread8_rep(pasemi->io, instr->ctx.data.buf.in,
				    instr->ctx.data.len);
		else
			pasemi_read_buf(chip, instr->ctx.data.buf.in,
					instr->ctx.data.len);
		return 0;

	case NAND_OP_DATA_OUT_INSTR:
		if (instr->ctx.data.force_8bit)
			iowrite8_rep(pasemi->io, instr->ctx.data.buf.out,
				     instr->ctx.data.len);
		else
			pasemi_write_buf(chip, instr->ctx.data.buf.out,
					 instr->ctx.data.len);
		return 0;

	case NAND_OP_WAITRDY_INSTR:
		return nand_poll(inl(lpcctl) & LBICTRL_LPCCTL_NR, 10, 10,
				 instr->ctx.waitrdy.timeout_ms, true);

	default:
		return -EINVAL;
	}

	return 0;
}

static int pasemi_exec_op(struct nand_chip *chip,
			  const struct nand_operation *op,
			  bool check_only)
{
	unsigned int i;
	int ret = 0;

	if (check_only)
		return 0;

	for (i = 0; i < op->ninstrs; i++) {
		ret = pasemi_exec_instr(chip, &op->instrs[i]);
		if (ret)
			break;

		if (op->instrs[i].delay_ns)
			ndelay(op->instrs[i].delay_ns);
	}

	return ret;
}

static const struct nand_controller_ops pasemi_ops = {
	.exec_op = pasemi_exec_op,
};

static int pasemi_nand_probe(struct platform_device *ofdev)
{
	struct pasemi_nand_controller *pasemi;
	struct device *dev = &ofdev->dev;
	struct pci_dev *pdev;
	struct device_node *np = dev->of_node;
	struct resource res;
	struct nand_chip *chip;
	int err = 0;

	err = of_address_to_resource(np, 0, &res);

	if (err)
		return -EINVAL;

	/* We only support one device at the moment */
	if (pasemi_nand_mtd)
		return -ENODEV;

	dev_dbg(dev, "pasemi_nand at %pR\n", &res);

	/* Allocate memory for NAND structure and private data */
	pasemi = devm_kzalloc(&ofdev->dev, sizeof(*pasemi), GFP_KERNEL);
	if (!pasemi)
		return -ENOMEM;

	nand_controller_init(&pasemi->base);
	pasemi->base.ops = &pasemi_ops;
	chip = &pasemi->chip;
	chip->controller = &pasemi->base;

	/* Link the private data with the MTD structure */
	pasemi_nand_mtd->dev.parent = dev;

	pasemi->io = of_iomap(np, 0);
	if (!pasemi->io)
		return -EIO;

	chip->legacy.IO_ADDR_R = pasemi->io;
	chip->legacy.IO_ADDR_W = pasemi->io;

	pdev = pci_get_device(PCI_VENDOR_ID_PASEMI, 0xa008, NULL);
	if (!pdev) {
		err = -ENODEV;
		goto out_ior;
	}

	lpcctl = pci_resource_start(pdev, 0);
	pci_dev_put(pdev);

	if (!request_region(lpcctl, 4, driver_name)) {
		err = -EBUSY;
		goto out_ior;
	}

	chip->legacy.cmd_ctrl = pasemi_hwcontrol;
	chip->legacy.dev_ready = pasemi_device_ready;
	chip->legacy.read_buf = pasemi_read_buf;
	chip->legacy.write_buf = pasemi_write_buf;
	chip->legacy.chip_delay = 0;
	chip->ecc.mode = NAND_ECC_SOFT;
	chip->ecc.algo = NAND_ECC_HAMMING;

	/* Enable the following for a flash based bad block table */
	chip->bbt_options = NAND_BBT_USE_FLASH;

	/* Scan to find existence of the device */
	err = nand_scan(chip, 1);
	if (err)
		goto out_lpc;

	if (mtd_device_register(pasemi_nand_mtd, NULL, 0)) {
		dev_err(dev, "Unable to register MTD device\n");
		err = -ENODEV;
		goto out_cleanup_nand;
	}

	pasemi_nand_mtd = nand_to_mtd(chip);
	dev_info(dev, "PA Semi NAND flash at %pR, control at I/O %x\n", &res,
		 lpcctl);

	return 0;

 out_cleanup_nand:
	nand_cleanup(chip);
 out_lpc:
	release_region(lpcctl, 4);
 out_ior:
	iounmap(pasemi->io);
	return err;
}

static int pasemi_nand_remove(struct platform_device *ofdev)
{
	struct pasemi_nand_controller *pasemi;
	struct nand_chip *chip;
	int ret;

	if (!pasemi_nand_mtd)
		return 0;

	chip = mtd_to_nand(pasemi_nand_mtd);
	pasemi = chip_to_pasemi(chip);

	/* Release resources, unregister device */
	ret = mtd_device_unregister(pasemi_nand_mtd);
	WARN_ON(ret);
	nand_cleanup(chip);

	release_region(lpcctl, 4);

	iounmap(pasemi->io);

	pasemi_nand_mtd = NULL;

	return 0;
}

static const struct of_device_id pasemi_nand_match[] =
{
	{
		.compatible   = "pasemi,localbus-nand",
	},
	{},
};

MODULE_DEVICE_TABLE(of, pasemi_nand_match);

static struct platform_driver pasemi_nand_driver =
{
	.driver = {
		.name = driver_name,
		.of_match_table = pasemi_nand_match,
	},
	.probe		= pasemi_nand_probe,
	.remove		= pasemi_nand_remove,
};

module_platform_driver(pasemi_nand_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Egor Martovetsky <egor@pasemi.com>");
MODULE_DESCRIPTION("NAND flash interface driver for PA Semi PWRficient");
