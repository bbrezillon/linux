/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2019 Macronix
 * Author: Miquèl Raynal <miquel.raynal@bootlin.com>
 *
 * Header for the Macronix external ECC engine.
 */

#ifndef __MTD_NAND_ECC_MXIC_H__
#define __MTD_NAND_ECC_MXIC_H__

#include <linux/device.h>

#if IS_ENABLED(CONFIG_MTD_NAND_ECC_MXIC)

bool mxic_ecc_use_engine(struct device *host_dev);
int mxic_ecc_data_xfer(struct device *host_dev);

#else /* !CONFIG_MTD_NAND_ECC_MXIC */

static inline bool mxic_ecc_use_engine(struct device *host_dev)
{
	return false;
}

static inline int mxic_ecc_data_xfer(struct device *host_dev)
{
	return -ENOTSUPP;
}

#endif /* CONFIG_MTD_NAND_ECC_MXIC */

#endif /* __MTD_NAND_ECC_MXIC_H__ */
