// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/wait.h>
#include <linux/mtd/spi-nor.h>

#include "internals.h"

static int micron_st_post_sfdp_fixups(struct spi_nor *nor,
				      struct spi_nor_flash_parameter *params)
{
	/* All ST/Micron NORs support the unlock/lock operations. */
	nor->flags |= SNOR_F_HAS_LOCK;
	nor->set_4byte = en4_ex4_wen_set_4byte;
	nor->quad_enable = no_quad_enable;

	return 0;
}

static const struct spi_nor_fixups micron_st_fixups = {
	.post_sfdp = micron_st_post_sfdp_fixups,
};

static const struct flash_info micron_parts[] = {
	{
		"mt35xu512aba",
		INFO(0x2c5b1a, 0, 128 * 1024, 512,
		     SECT_4K | USE_FSR | SPI_NOR_4B_OPCODES)
	},
};

const struct spi_nor_manufacturer spi_nor_micron = {
	.name = "micron",
	.parts = micron_parts,
	.nparts = ARRAY_SIZE(micron_parts),
	.fixups = &micron_st_fixups,
};

static const struct flash_info st_parts[] = {
	{
		"n25q016a",
		INFO(0x20bb15, 0, 64 * 1024, 32, SECT_4K | SPI_NOR_QUAD_READ)
	},
	{
		"n25q032",
		INFO(0x20ba16, 0, 64 * 1024, 64, SPI_NOR_QUAD_READ)
	},
	{
		"n25q032a",
		INFO(0x20bb16, 0, 64 * 1024, 64, SPI_NOR_QUAD_READ)
	},
	{
		"n25q064",
		INFO(0x20ba17, 0, 64 * 1024, 128, SECT_4K | SPI_NOR_QUAD_READ)
	},
	{
		"n25q064a",
		INFO(0x20bb17, 0, 64 * 1024, 128, SECT_4K | SPI_NOR_QUAD_READ)
	},
	{
		"n25q128a11",
		INFO(0x20bb18, 0, 64 * 1024, 256, SECT_4K | SPI_NOR_QUAD_READ)
	},
	{
		"n25q128a13",
		INFO(0x20ba18, 0, 64 * 1024, 256, SECT_4K | SPI_NOR_QUAD_READ)
	},
	{
		"n25q256a",
		INFO(0x20ba19, 0, 64 * 1024, 512,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"n25q256ax1",
		INFO(0x20bb19, 0, 64 * 1024, 512, SECT_4K | SPI_NOR_QUAD_READ)
	},
	{
		"n25q512a",
		INFO(0x20bb20, 0, 64 * 1024, 1024,
		     SECT_4K | USE_FSR | SPI_NOR_QUAD_READ)
	},
	{
		"n25q512ax3",
		INFO(0x20ba20, 0, 64 * 1024, 1024,
		     SECT_4K | USE_FSR | SPI_NOR_QUAD_READ)
	},
	{
		"n25q00",
		INFO(0x20ba21, 0, 64 * 1024, 2048,
		     SECT_4K | USE_FSR | SPI_NOR_QUAD_READ | NO_CHIP_ERASE)
	},
	{
		"n25q00a",
		INFO(0x20bb21, 0, 64 * 1024, 2048,
		     SECT_4K | USE_FSR | SPI_NOR_QUAD_READ | NO_CHIP_ERASE)
	},
	{
		"mt25qu02g",
		INFO(0x20bb22, 0, 64 * 1024, 4096,
		     SECT_4K | USE_FSR | SPI_NOR_QUAD_READ | NO_CHIP_ERASE)
	},
	{ "m25p05",  INFO(0x202010,  0,  32 * 1024,   2, 0) },
	{ "m25p10",  INFO(0x202011,  0,  32 * 1024,   4, 0) },
	{ "m25p20",  INFO(0x202012,  0,  64 * 1024,   4, 0) },
	{ "m25p40",  INFO(0x202013,  0,  64 * 1024,   8, 0) },
	{ "m25p80",  INFO(0x202014,  0,  64 * 1024,  16, 0) },
	{ "m25p16",  INFO(0x202015,  0,  64 * 1024,  32, 0) },
	{ "m25p32",  INFO(0x202016,  0,  64 * 1024,  64, 0) },
	{ "m25p64",  INFO(0x202017,  0,  64 * 1024, 128, 0) },
	{ "m25p128", INFO(0x202018,  0, 256 * 1024,  64, 0) },
};

const struct spi_nor_manufacturer spi_nor_st = {
	.name = "st",
	.parts = st_parts,
	.nparts = ARRAY_SIZE(st_parts),
	.fixups = &micron_st_fixups,
};
