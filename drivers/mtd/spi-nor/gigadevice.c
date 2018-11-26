// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/wait.h>
#include <linux/mtd/spi-nor.h>

#include "internals.h"

static int gd25q256_post_sfdp_fixups(struct spi_nor *nor,
                                     struct spi_nor_flash_parameter *params)
{
	params->quad_enable = sr1_bit6_quad_enable;

	return 0;
}

static const struct spi_nor_fixups gd25q256_fixups = {
	.post_sfdp = gd25q256_post_sfdp_fixups,
};

static const struct flash_info gigadevice_parts[] = {
	{
		"gd25q16",
		INFO(0xc84015, 0, 64 * 1024,  32,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"gd25q32",
		INFO(0xc84016, 0, 64 * 1024,  64,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"gd25lq32",
		INFO(0xc86016, 0, 64 * 1024, 64,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"gd25q64",
		INFO(0xc84017, 0, 64 * 1024, 128,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"gd25lq64c",
		INFO(0xc86017, 0, 64 * 1024, 128,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"gd25q128",
		INFO(0xc84018, 0, 64 * 1024, 256,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"gd25q256",
		INFO(0xc84019, 0, 64 * 1024, 512,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_4B_OPCODES | SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
		.fixups = &gd25q256_fixups,
	},
};

const struct spi_nor_manufacturer spi_nor_gigadevice = {
	.name = "gigadevice",
	.parts = gigadevice_parts,
	.nparts = ARRAY_SIZE(gigadevice_parts),
};
