// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/wait.h>
#include <linux/mtd/spi-nor.h>

#include "internals.h"

static const struct flash_info winbond_parts[] = {
	{ "w25x05", INFO(0xef3010, 0, 64 * 1024, 1, SECT_4K) },
	{ "w25x10", INFO(0xef3011, 0, 64 * 1024, 2, SECT_4K) },
	{ "w25x20", INFO(0xef3012, 0, 64 * 1024, 4, SECT_4K) },
	{ "w25x40", INFO(0xef3013, 0, 64 * 1024, 8, SECT_4K) },
	{ "w25x80", INFO(0xef3014, 0, 64 * 1024, 16, SECT_4K) },
	{ "w25x16", INFO(0xef3015, 0, 64 * 1024, 32, SECT_4K) },
	{
		"w25q16dw",
		INFO(0xef6015, 0, 64 * 1024,  32,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{ "w25x32", INFO(0xef3016, 0, 64 * 1024, 64, SECT_4K) },
	{ "w25q20cl", INFO(0xef4012, 0, 64 * 1024, 4, SECT_4K) },
	{ "w25q20bw", INFO(0xef5012, 0, 64 * 1024, 4, SECT_4K) },
	{ "w25q20ew", INFO(0xef6012, 0, 64 * 1024, 4, SECT_4K) },
	{ "w25q32", INFO(0xef4016, 0, 64 * 1024, 64, SECT_4K) },
	{
		"w25q32dw",
		INFO(0xef6016, 0, 64 * 1024,  64,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"w25q32jv",
		INFO(0xef7016, 0, 64 * 1024, 64,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{ "w25x64", INFO(0xef3017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q64", INFO(0xef4017, 0, 64 * 1024, 128, SECT_4K) },
	{
		"w25q64dw",
		INFO(0xef6017, 0, 64 * 1024, 128,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"w25q128fw",
		INFO(0xef6018, 0, 64 * 1024, 256,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"w25q128jv",
		INFO(0xef7018, 0, 64 * 1024, 256,
			SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
			SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{ "w25q80", INFO(0xef5014, 0, 64 * 1024, 16, SECT_4K) },
	{ "w25q80bl", INFO(0xef4014, 0, 64 * 1024, 16, SECT_4K) },
	{ "w25q128", INFO(0xef4018, 0, 64 * 1024, 256, SECT_4K) },
	{
		"w25q256",
		INFO(0xef4019, 0, 64 * 1024, 512,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"w25m512jv",
		INFO(0xef7119, 0, 64 * 1024, 1024,
		     SECT_4K | SPI_NOR_QUAD_READ | SPI_NOR_DUAL_READ)
	},
};

static int winbond_set_4byte(struct spi_nor *nor, bool enable)
{
	int ret;

	ret = en4_ex4_set_4byte(nor, enable);
	if (ret || enable)
		return ret;

	/*
	 * On Winbond W25Q256FV, leaving 4byte mode causes the Extended Address
	 * Register to be set to 1, so all 3-byte-address reads come from the
	 * second 16M.
	 * We must clear the register to enable normal behavior.
	 */
	write_enable(nor);
	nor->cmd_buf[0] = 0;
	nor->write_reg(nor, SPINOR_OP_WREAR, nor->cmd_buf, 1);
	write_disable(nor);

	return ret;
}

static int winbond_post_sfdp_fixups(struct spi_nor *nor,
				    struct spi_nor_flash_parameter *params)
{
	nor->set_4byte = winbond_set_4byte;

	return 0;
}

static const struct spi_nor_fixups winbond_fixups = {
	.post_sfdp = winbond_post_sfdp_fixups,
};

const struct spi_nor_manufacturer spi_nor_winbond = {
	.name = "winbond",
	.parts = winbond_parts,
	.nparts = ARRAY_SIZE(winbond_parts),
	.fixups = &winbond_fixups,
};
