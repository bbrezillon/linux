// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/sizes.h>
#include <linux/wait.h>
#include <linux/mtd/spi-nor.h>

#include "internals.h"

static const struct flash_info spansion_parts[] = {
	{
		"s25sl032p",
		INFO(0x010215, 0x4d00, 64 * 1024, 64,
		     SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"s25sl064p",
		INFO(0x010216, 0x4d00, 64 * 1024, 128,
		     SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"s25fl256s0",
		INFO(0x010219, 0x4d00, 256 * 1024, 128, USE_CLSR)
	},
	{
		"s25fl256s1",
		INFO(0x010219, 0x4d01, 64 * 1024, 512,
		     SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ | USE_CLSR)
	},
	{
		"s25fl512s",
		INFO(0x010220, 0x4d00, 256 * 1024, 256,
		     SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ | USE_CLSR)
	},
	{ "s70fl01gs", INFO(0x010221, 0x4d00, 256 * 1024, 256, 0) },
	{ "s25sl12800", INFO(0x012018, 0x0300, 256 * 1024,  64, 0) },
	{ "s25sl12801", INFO(0x012018, 0x0301, 64 * 1024, 256, 0) },
	{
		"s25fl128s",
		INFO6(0x012018, 0x4d0180, 64 * 1024, 256,
		      SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ | USE_CLSR)
	},
	{
		"s25fl129p0",
		INFO(0x012018, 0x4d00, 256 * 1024, 64,
		     SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ | USE_CLSR)
	},
	{
		"s25fl129p1",
		INFO(0x012018, 0x4d01, 64 * 1024, 256,
		     SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ | USE_CLSR)
	},
	{ "s25sl004a", INFO(0x010212, 0,  64 * 1024,   8, 0) },
	{ "s25sl008a", INFO(0x010213, 0,  64 * 1024,  16, 0) },
	{ "s25sl016a", INFO(0x010214, 0,  64 * 1024,  32, 0) },
	{ "s25sl032a", INFO(0x010215, 0,  64 * 1024,  64, 0) },
	{ "s25sl064a", INFO(0x010216, 0,  64 * 1024, 128, 0) },
	{
		"s25fl004k",
		INFO(0xef4013, 0, 64 * 1024, 8,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"s25fl008k",
		INFO(0xef4014, 0, 64 * 1024, 16,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"s25fl016k",
		INFO(0xef4015, 0, 64 * 1024, 32,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{ "s25fl064k", INFO(0xef4017, 0, 64 * 1024, 128, SECT_4K) },
	{
		"s25fl116k",
		INFO(0x014015, 0, 64 * 1024, 32,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{ "s25fl132k", INFO(0x014016, 0, 64 * 1024, 64, SECT_4K) },
	{ "s25fl164k", INFO(0x014017, 0, 64 * 1024, 128, SECT_4K) },
	{
		"s25fl204k",
		INFO(0x014013, 0, 64 * 1024, 8, SECT_4K | SPI_NOR_DUAL_READ)
	},
	{
		"s25fl208k",
		INFO(0x014014, 0, 64 * 1024, 16, SECT_4K | SPI_NOR_DUAL_READ)
	},
	{
		"s25fl064l",
		INFO(0x016017, 0, 64 * 1024, 128,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_4B_OPCODES)
	},
	{
		"s25fl128l",
		INFO(0x016018, 0, 64 * 1024, 256,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_4B_OPCODES)
	},
	{
		"s25fl256l",
		INFO(0x016019, 0, 64 * 1024, 512,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
		     SPI_NOR_4B_OPCODES)
	},
};

static int spansion_post_sfdp_fixups(struct spi_nor *nor,
				     struct spi_nor_flash_parameter *params)
{
	struct mtd_info *mtd = &nor->mtd;

	if (mtd->size > SZ_16M) {
		nor->flags |= SNOR_F_4B_OPCODES;

		/* No small sector erase for 4-byte command set */
		nor->erase_opcode = SPINOR_OP_SE;
		nor->mtd.erasesize = nor->info->sector_size;
	}

	return 0;
}

static const struct spi_nor_fixups spansion_fixups = {
	.post_sfdp = spansion_post_sfdp_fixups,
};

const struct spi_nor_manufacturer spi_nor_spansion = {
	.name = "spansion",
	.parts = spansion_parts,
	.nparts = ARRAY_SIZE(spansion_parts),
	.fixups = &spansion_fixups,
};
