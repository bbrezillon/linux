// SPDX-License-Identifier: GPL-2.0+
/*
 * Generic Error-Correcting Code (ECC) engine
 *
 * Copyright (C) 2019 Macronix
 * Author:
 *     Miquèl RAYNAL <miquel.raynal@bootlin.com>
 *
 *
 * This file describes the abstraction of any NAND ECC engine. It has been
 * designed to fit most cases, including parallel NANDs and SPI-NANDs.
 *
 * There are three main situations where instantiating this ECC engine makes
 * sense:
 *   - "external": The ECC engine is outside the NAND pipeline, typically this
 *                 is a software ECC engine. One can also imagine a generic
 *                 hardware ECC engine which would be an IP itself. Interacting
 *                 with a SPI-NAND device without on-die ECC could be achieved
 *                 thanks to the use of such external engine.
 *   - "pipelined": The ECC engine is inside the NAND pipeline, ie. on the
 *                  controller's side. This is the case of most of the raw NAND
 *                  controllers. These controllers usually embed an hardware ECC
 *                  engine which is managed thanks to the same register set as
 *                  the controller's.
 *   - "ondie": The ECC engine is inside the NAND pipeline, on the chip's side.
 *              Some NAND chips can correct themselves the data. The on-die
 *              correction can be enabled, disabled and the status of the
 *              correction after a read may be retrieved with a NAND command
 *              (may be vendor specific).
 *
 * Besides the initial setup and final cleanups, the interfaces are rather
 * simple:
 *   - "prepare": Prepare an I/O request, check the ECC engine is enabled or
 *                disabled as requested before the I/O. In case of software
 *                correction, this step may involve to derive the ECC bytes and
 *                place them in the OOB area before a write.
 *   - "finish": Finish an I/O request, check the status of the operation ie.
 *               the data validity in case of a read (report to the upper layer
 *               any bitflip/errors).
 *
 * Both prepare/finish callbacks are supposed to enclose I/O request and will
 * behave differently depending on the desired correction:
 *   - "raw": Correction disabled
 *   - "ecc": Correction enabled
 *
 * The request direction is impacting the logic as well:
 *   - "read": Load data from the NAND chip
 *   - "write": Store data in the NAND chip
 *
 * Mixing all this combinations together gives the following behavior.
 *
 * ["external" ECC engine]
 *   - external + prepare + raw + read: do nothing
 *   - external + finish  + raw + read: do nothing
 *   - external + prepare + raw + write: do nothing
 *   - external + finish  + raw + write: do nothing
 *   - external + prepare + ecc + read: do nothing
 *   - external + finish  + ecc + read: calculate expected ECC bytes, extract
 *                                      ECC bytes from OOB buffer, correct
 *                                      and report any bitflip/error
 *   - external + prepare + ecc + write: calculate ECC bytes and store them at
 *                                       the right place in the OOB buffer based
 *                                       on the OOB layout
 *   - external + finish  + ecc + write: do nothing
 *
 * ["pipelined" ECC engine]
 *   - pipelined + prepare + raw + read: disable the controller's ECC engine if
 *                                       activated
 *   - pipelined + finish  + raw + read: do nothing
 *   - pipelined + prepare + raw + write: disable the controller's ECC engine if
 *                                        activated
 *   - pipelined + finish  + raw + write: do nothing
 *   - pipelined + prepare + ecc + read: enable the controller's ECC engine if
 *                                       deactivated
 *   - pipelined + finish  + ecc + read: check the status, report any
 *                                       error/bitflip
 *   - pipelined + prepare + ecc + write: enable the controller's ECC engine if
 *                                        deactivated
 *   - pipelined + finish  + ecc + write: do nothing
 *
 * ["ondie" ECC engine]
 *   - ondie + prepare + raw + read: send commands to disable the on-chip ECC
 *                                   engine if activated
 *   - ondie + finish  + raw + read: do nothing
 *   - ondie + prepare + raw + write: send commands to disable the on-chip ECC
 *                                    engine if activated
 *   - ondie + finish  + raw + write: do nothing
 *   - ondie + prepare + ecc + read: send commands to enable the on-chip ECC
 *                                   engine if deactivated
 *   - ondie + finish  + ecc + read: send commands to check the status, report
 *                                   any error/bitflip
 *   - ondie + prepare + ecc + write: send commands to enable the on-chip ECC
 *                                    engine if deactivated
 *   - ondie + finish  + ecc + write: do nothing
 */

#include <linux/module.h>
#include <linux/mtd/nand.h>

int nand_ecc_init_ctx(struct nand_device *nand)
{
	if (!nand->ecc.engine->ops->init_ctx)
		return 0;

	return nand->ecc.engine->ops->init_ctx(nand);
}
EXPORT_SYMBOL(nand_ecc_init_ctx);

void nand_ecc_cleanup_ctx(struct nand_device *nand)
{
	if (nand->ecc.engine->ops->cleanup_ctx)
		nand->ecc.engine->ops->cleanup_ctx(nand);
}
EXPORT_SYMBOL(nand_ecc_cleanup_ctx);

int nand_ecc_prepare_io_req(struct nand_device *nand,
			    struct nand_page_io_req *req)
{
	if (!nand->ecc.engine->ops->prepare_io_req)
		return 0;

	return nand->ecc.engine->ops->prepare_io_req(nand, req);
}
EXPORT_SYMBOL(nand_ecc_prepare_io_req);

int nand_ecc_finish_io_req(struct nand_device *nand,
			   struct nand_page_io_req *req)
{
	if (!nand->ecc.engine->ops->finish_io_req)
		return 0;

	return nand->ecc.engine->ops->finish_io_req(nand, req);
}
EXPORT_SYMBOL(nand_ecc_finish_io_req);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miquel Raynal <miquel.raynal@bootlin.com>");
MODULE_DESCRIPTION("Generic ECC engine");
