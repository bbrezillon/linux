// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/wait.h>
#include <linux/mtd/spi-nor.h>

#include "internals.h"

static const struct flash_info intel_parts[] = {
	{ "160s33b",  INFO(0x898911, 0, 64 * 1024,  32, 0) },
        { "320s33b",  INFO(0x898912, 0, 64 * 1024,  64, 0) },
        { "640s33b",  INFO(0x898913, 0, 64 * 1024, 128, 0) },
};

static int intel_post_sfdp_fixups(struct spi_nor *nor, 
				  struct spi_nor_flash_parameter *params)
{
	nor->flags |= SNOR_F_CLR_SW_PROT_BITS;

	return 0;
}

static const struct spi_nor_fixups intel_fixups = {
	.post_sfdp = intel_post_sfdp_fixups,
};

const struct spi_nor_manufacturer spi_nor_intel = {
	.name = "intel",
	.parts = intel_parts,
	.nparts = ARRAY_SIZE(intel_parts),
	.fixups = &intel_fixups,
};
