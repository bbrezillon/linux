// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * arch/arm/mach-ep93xx/snappercl15.c
 * Bluewater Systems Snapper CL15 system module
 *
 * Copyright (C) 2009 Bluewater Systems Ltd
 * Author: Ryan Mallon
 *
 * NAND code adapted from driver by:
 *   Andre Renaud <andre@bluewatersys.com>
 *   James R. McKaskill
 */

#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/fb.h>

#include <linux/mtd/platnand.h>

#include "hardware.h"
#include <linux/platform_data/video-ep93xx.h>
#include "gpio-ep93xx.h"

#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include "soc.h"

#define SNAPPERCL15_NAND_BASE	(EP93XX_CS7_PHYS_BASE + SZ_16M)

#define SNAPPERCL15_NAND_WPN	(1 << 8)  /* Write protect (active low) */
#define SNAPPERCL15_NAND_ALE	(1 << 9)  /* Address latch */
#define SNAPPERCL15_NAND_CLE	(1 << 10) /* Command latch */
#define SNAPPERCL15_NAND_CEN	(1 << 11) /* Chip enable (active low) */
#define SNAPPERCL15_NAND_RDY	(1 << 14) /* Device ready */

#define NAND_CTRL_ADDR(chip) 	(plat_nand_get_io_base(chip) + 0x40)
#define NAND_CTRL_DATA(chip) 	plat_nand_get_io_base(chip)

static struct mtd_partition snappercl15_nand_parts[] = {
	{
		.name		= "Kernel",
		.offset		= 0,
		.size		= SZ_2M,
	},
	{
		.name		= "Filesystem",
		.offset		= MTDPART_OFS_APPEND,
		.size		= MTDPART_SIZ_FULL,
	},
};

static int snappercl15_nand_exec_instr(struct nand_chip *chip,
				       const struct nand_op_instr *instr)
{
	unsigned int i;

	switch (instr->type) {
	case NAND_OP_CMD_INSTR:
		__raw_writew(SNAPPERCL15_NAND_WPN | SNAPPERCL15_NAND_CLE,
			     NAND_CTRL_ADDR(chip));
		__raw_writew(instr->ctx.cmd.opcode |
			     SNAPPERCL15_NAND_WPN | SNAPPERCL15_NAND_CLE,
			     NAND_CTRL_DATA(chip));
		return 0;

	case NAND_OP_ADDR_INSTR:
		__raw_writew(SNAPPERCL15_NAND_WPN | SNAPPERCL15_NAND_ALE,
			     NAND_CTRL_ADDR(chip));

		for (i = 0; i < instr->ctx.addr.naddrs; i++)
			__raw_writew(instr->ctx.addr.addrs[i] |
				     SNAPPERCL15_NAND_WPN | SNAPPERCL15_NAND_ALE,
				     NAND_CTRL_DATA(chip));
		return 0;

	case NAND_OP_DATA_IN_INSTR:
		__raw_writew(SNAPPERCL15_NAND_WPN, NAND_CTRL_ADDR(chip));
		ioread8_rep(NAND_CTRL_DATA(chip), instr->ctx.data.buf.in,
			    instr->ctx.data.len);
		return 0;

	case NAND_OP_DATA_OUT_INSTR:
		__raw_writew(SNAPPERCL15_NAND_WPN, NAND_CTRL_ADDR(chip));
		iowrite8_rep(NAND_CTRL_DATA(chip), instr->ctx.data.buf.out,
			     instr->ctx.data.len);
		return 0;

	case NAND_OP_WAITRDY_INSTR:
		return nand_poll(__raw_readw(NAND_CTRL_ADDR(chip)) & SNAPPERCL15_NAND_RDY,
				 0, 0, instr->ctx.waitrdy.timeout_ms, false);

	default:
		break;
	}

	return -EINVAL;
}

static int snappercl15_nand_exec_op(struct nand_chip *chip,
				    const struct nand_operation *op,
				    bool check_only)
{
	int ret = 0;

	if (check_only)
		return true;

	__raw_writew(SNAPPERCL15_NAND_WPN, NAND_CTRL_ADDR(chip));
	for (unsigned int i = 0; i < op->ninstrs; i++) {
		ret = snappercl15_nand_exec_instr(chip, &op->instrs[i]);
		if (ret)
			break;

		if (op->instrs[i].delay_us)
			udelay(op->instrs[i].delay_us);

	}
	__raw_writew(SNAPPERCL15_NAND_NCE | SNAPPERCL15_NAND_WPN,
		     NAND_CTRL_ADDR(chip));

	return ret;
}

static const struct nand_controller_ops snappercl15_nand_ops = {
	.exec_op = snappercl15_nand_exec_op,
};

static struct platform_nand_data snappercl15_nand_data = {
	.chip = {
		.nr_chips		= 1,
		.partitions		= snappercl15_nand_parts,
		.nr_partitions		= ARRAY_SIZE(snappercl15_nand_parts),
	},
	.ctrl = {
		.ops			= &snappercl15_nand_ops;
	},
};

static struct resource snappercl15_nand_resource[] = {
	{
		.start		= SNAPPERCL15_NAND_BASE,
		.end		= SNAPPERCL15_NAND_BASE + SZ_4K - 1,
		.flags		= IORESOURCE_MEM,
	},
};

static struct platform_device snappercl15_nand_device = {
	.name			= "gen_nand",
	.id			= -1,
	.dev.platform_data	= &snappercl15_nand_data,
	.resource		= snappercl15_nand_resource,
	.num_resources		= ARRAY_SIZE(snappercl15_nand_resource),
};

static struct ep93xx_eth_data __initdata snappercl15_eth_data = {
	.phy_id			= 1,
};

static struct i2c_board_info __initdata snappercl15_i2c_data[] = {
	{
		/* Audio codec */
		I2C_BOARD_INFO("tlv320aic23", 0x1a),
	},
};

static struct ep93xxfb_mach_info __initdata snappercl15_fb_info = {
};

static struct platform_device snappercl15_audio_device = {
	.name		= "snappercl15-audio",
	.id		= -1,
};

static void __init snappercl15_register_audio(void)
{
	ep93xx_register_i2s();
	platform_device_register(&snappercl15_audio_device);
}

static void __init snappercl15_init_machine(void)
{
	ep93xx_init_devices();
	ep93xx_register_eth(&snappercl15_eth_data, 1);
	ep93xx_register_i2c(snappercl15_i2c_data,
			    ARRAY_SIZE(snappercl15_i2c_data));
	ep93xx_register_fb(&snappercl15_fb_info);
	snappercl15_register_audio();
	platform_device_register(&snappercl15_nand_device);
}

MACHINE_START(SNAPPER_CL15, "Bluewater Systems Snapper CL15")
	/* Maintainer: Ryan Mallon */
	.atag_offset	= 0x100,
	.map_io		= ep93xx_map_io,
	.init_irq	= ep93xx_init_irq,
	.init_time	= ep93xx_timer_init,
	.init_machine	= snappercl15_init_machine,
	.init_late	= ep93xx_init_late,
	.restart	= ep93xx_restart,
MACHINE_END
