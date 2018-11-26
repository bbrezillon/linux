// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/wait.h>
#include <linux/mtd/spi-nor.h>

#include "internals.h"

static const struct flash_info issi_parts[] = {
	/* ISSI */
	{ "is25cd512",  INFO(0x7f9d20, 0, 32 * 1024,   2, SECT_4K) },
	{
		"is25lq040b",
		INFO(0x9d4013, 0, 64 * 1024,   8,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"is25lp080d",
		INFO(0x9d6014, 0, 64 * 1024,  16,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"is25lp032",
		INFO(0x9d6016, 0, 64 * 1024,  64, SECT_4K | SPI_NOR_DUAL_READ)
	},
        {
		"is25lp064",
		INFO(0x9d6017, 0, 64 * 1024, 128, SECT_4K | SPI_NOR_DUAL_READ)
	},
	{
		"is25lp128",
		INFO(0x9d6018, 0, 64 * 1024, 256, SECT_4K | SPI_NOR_DUAL_READ)
	},
	{
		"is25lp256",
		INFO(0x9d6019, 0, 64 * 1024, 512, SECT_4K | SPI_NOR_DUAL_READ)
	},
	{
		"is25wp032",
		INFO(0x9d7016, 0, 64 * 1024,  64,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"is25wp064",
		INFO(0x9d7017, 0, 64 * 1024, 128,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},
	{
		"is25wp128",
		INFO(0x9d7018, 0, 64 * 1024, 256,
		     SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)
	},

	/* PMC */
	{ "pm25lv512",   INFO(0, 0, 32 * 1024, 2, SECT_4K_PMC) },
	{ "pm25lv010",   INFO(0, 0, 32 * 1024, 4, SECT_4K_PMC) },
	{ "pm25lq032",   INFO(0x7f9d46, 0, 64 * 1024, 64, SECT_4K) },
};

const struct spi_nor_manufacturer spi_nor_issi = {
	.name = "issi",
	.parts = issi_parts,
	.nparts = ARRAY_SIZE(issi_parts),
};
