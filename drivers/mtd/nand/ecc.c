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
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

static LIST_HEAD(hw_engines);
static DEFINE_MUTEX(hw_engines_mutex);

int nand_ecc_init_ctx(struct nand_device *nand)
{
	if (!nand->ecc.engine || !nand->ecc.engine->ops->init_ctx)
		return 0;

	return nand->ecc.engine->ops->init_ctx(nand);
}
EXPORT_SYMBOL(nand_ecc_init_ctx);

void nand_ecc_cleanup_ctx(struct nand_device *nand)
{
	if (nand->ecc.engine && nand->ecc.engine->ops->cleanup_ctx)
		nand->ecc.engine->ops->cleanup_ctx(nand);
}
EXPORT_SYMBOL(nand_ecc_cleanup_ctx);

int nand_ecc_prepare_io_req(struct nand_device *nand,
			    struct nand_page_io_req *req)
{
	if (!nand->ecc.engine || !nand->ecc.engine->ops->prepare_io_req)
		return 0;

	return nand->ecc.engine->ops->prepare_io_req(nand, req);
}
EXPORT_SYMBOL(nand_ecc_prepare_io_req);

int nand_ecc_finish_io_req(struct nand_device *nand,
			   struct nand_page_io_req *req)
{
	if (!nand->ecc.engine || !nand->ecc.engine->ops->finish_io_req)
		return 0;

	return nand->ecc.engine->ops->finish_io_req(nand, req);
}
EXPORT_SYMBOL(nand_ecc_finish_io_req);

/* Define default oob placement schemes for large and small page devices */
static int nand_ooblayout_ecc_sp(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	unsigned int total_ecc_bytes = nand->ecc.ctx.total;

	if (section > 1)
		return -ERANGE;

	if (!section) {
		oobregion->offset = 0;
		if (mtd->oobsize == 16)
			oobregion->length = 4;
		else
			oobregion->length = 3;
	} else {
		if (mtd->oobsize == 8)
			return -ERANGE;

		oobregion->offset = 6;
		oobregion->length = total_ecc_bytes - 4;
	}

	return 0;
}

static int nand_ooblayout_free_sp(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	if (section > 1)
		return -ERANGE;

	if (mtd->oobsize == 16) {
		if (section)
			return -ERANGE;

		oobregion->length = 8;
		oobregion->offset = 8;
	} else {
		oobregion->length = 2;
		if (!section)
			oobregion->offset = 3;
		else
			oobregion->offset = 6;
	}

	return 0;
}

const struct mtd_ooblayout_ops nand_ooblayout_sp_ops = {
	.ecc = nand_ooblayout_ecc_sp,
	.free = nand_ooblayout_free_sp,
};
EXPORT_SYMBOL_GPL(nand_ooblayout_sp_ops);

static int nand_ooblayout_ecc_lp(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	unsigned int total_ecc_bytes = nand->ecc.ctx.total;

	if (section || !total_ecc_bytes)
		return -ERANGE;

	oobregion->length = total_ecc_bytes;
	oobregion->offset = mtd->oobsize - oobregion->length;

	return 0;
}

static int nand_ooblayout_free_lp(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	unsigned int total_ecc_bytes = nand->ecc.ctx.total;

	if (section)
		return -ERANGE;

	oobregion->length = mtd->oobsize - total_ecc_bytes - 2;
	oobregion->offset = 2;

	return 0;
}

const struct mtd_ooblayout_ops nand_ooblayout_lp_ops = {
	.ecc = nand_ooblayout_ecc_lp,
	.free = nand_ooblayout_free_lp,
};
EXPORT_SYMBOL_GPL(nand_ooblayout_lp_ops);

/*
 * Support the old "large page" layout used for 1-bit Hamming ECC where ECC
 * are placed at a fixed offset.
 */
static int nand_ooblayout_ecc_lp_hamming(struct mtd_info *mtd, int section,
					 struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	unsigned int total_ecc_bytes = nand->ecc.ctx.total;

	if (section)
		return -ERANGE;

	switch (mtd->oobsize) {
	case 64:
		oobregion->offset = 40;
		break;
	case 128:
		oobregion->offset = 80;
		break;
	default:
		return -EINVAL;
	}

	oobregion->length = total_ecc_bytes;
	if (oobregion->offset + oobregion->length > mtd->oobsize)
		return -ERANGE;

	return 0;
}

static int nand_ooblayout_free_lp_hamming(struct mtd_info *mtd, int section,
					  struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	unsigned int total_ecc_bytes = nand->ecc.ctx.total;
	int ecc_offset = 0;

	if (section < 0 || section > 1)
		return -ERANGE;

	switch (mtd->oobsize) {
	case 64:
		ecc_offset = 40;
		break;
	case 128:
		ecc_offset = 80;
		break;
	default:
		return -EINVAL;
	}

	if (section == 0) {
		oobregion->offset = 2;
		oobregion->length = ecc_offset - 2;
	} else {
		oobregion->offset = ecc_offset + total_ecc_bytes;
		oobregion->length = mtd->oobsize - oobregion->offset;
	}

	return 0;
}

const struct mtd_ooblayout_ops nand_ooblayout_lp_hamming_ops = {
	.ecc = nand_ooblayout_ecc_lp_hamming,
	.free = nand_ooblayout_free_lp_hamming,
};
EXPORT_SYMBOL_GPL(nand_ooblayout_lp_hamming_ops);

static const char * const nand_ecc_engine_providers[] = {
	[NAND_ECC_ENGINE_NONE] = "none",
	[NAND_ECC_ENGINE_SOFT] = "soft",
	[NAND_ECC_ENGINE_CONTROLLER] = "hw",
	[NAND_ECC_ENGINE_ON_DIE] = "on-die",
};

static const char * const nand_ecc_placement[] = {
	[NAND_ECC_PLACEMENT_INTERLEAVED] = "interleaved",
};

static enum nand_ecc_engine_type
of_get_nand_ecc_engine_type(struct device_node *np)
{
	enum nand_ecc_engine_type eng_type;
	const char *pm;
	int err;

	err = of_property_read_string(np, "nand-ecc-provider", &pm);
	if (err)
		err = of_property_read_string(np, "nand-ecc-mode", &pm);
	if (err)
		return NAND_ECC_ENGINE_INVALID;

	for (eng_type = NAND_ECC_ENGINE_NONE;
	     eng_type < ARRAY_SIZE(nand_ecc_engine_providers); eng_type++) {
		if (!strcasecmp(pm, nand_ecc_engine_providers[eng_type]))
			return eng_type;
	}

	/*
	 * For backward compatibility we support few obsoleted values that don't
	 * have their mappings into the nand_ecc_engine_providers enum anymore
	 * (they were merged with other enums).
	 */
	if (!strcasecmp(pm, "soft_bch"))
		return NAND_ECC_ENGINE_SOFT;

	if (!strcasecmp(pm, "hw_syndrome"))
		return NAND_ECC_ENGINE_CONTROLLER;

	return NAND_ECC_ENGINE_INVALID;
}

enum nand_ecc_placement of_get_nand_ecc_placement(struct device_node *np)
{
	enum nand_ecc_placement placement;
	const char *pm;
	int err;

	err = of_property_read_string(np, "nand-ecc-placement", &pm);
	if (!err) {
		for (placement = NAND_ECC_PLACEMENT_INTERLEAVED;
		     placement < ARRAY_SIZE(nand_ecc_placement); placement++) {
			if (!strcasecmp(pm, nand_ecc_placement[placement]))
				return placement;
		}
	}

	/*
	 * For backward compatibility we support few obsoleted values that don't
	 * have their mappings into the nand_ecc_placement enum anymore.
	 */
	err = of_property_read_string(np, "nand-ecc-mode", &pm);
	if (!err) {
		if (!strcasecmp(pm, "hw_syndrome"))
			return NAND_ECC_PLACEMENT_INTERLEAVED;
	}

	return NAND_ECC_PLACEMENT_FREE;
}

static const char * const nand_ecc_algos[] = {
	[NAND_ECC_HAMMING]	= "hamming",
	[NAND_ECC_BCH]		= "bch",
	[NAND_ECC_RS]		= "rs",
};

static enum nand_ecc_algo of_get_nand_ecc_algo(struct device_node *np)
{
	enum nand_ecc_algo ecc_algo;
	const char *pm;
	int err;

	err = of_property_read_string(np, "nand-ecc-algo", &pm);
	if (!err) {
		for (ecc_algo = NAND_ECC_HAMMING;
		     ecc_algo < ARRAY_SIZE(nand_ecc_algos);
		     ecc_algo++) {
			if (!strcasecmp(pm, nand_ecc_algos[ecc_algo]))
				return ecc_algo;
		}
	}

	/*
	 * For backward compatibility we also read "nand-ecc-mode" checking
	 * for some obsoleted values that were specifying ECC algorithm.
	 */
	err = of_property_read_string(np, "nand-ecc-mode", &pm);
	if (!err) {
		if (!strcasecmp(pm, "soft"))
			return NAND_ECC_HAMMING;
		else if (!strcasecmp(pm, "soft_bch"))
			return NAND_ECC_BCH;
	}

	return NAND_ECC_UNKNOWN;
}

static int of_get_nand_ecc_step_size(struct device_node *np)
{
	int ret;
	u32 val;

	ret = of_property_read_u32(np, "nand-ecc-step-size", &val);
	return ret ? ret : val;
}

static int of_get_nand_ecc_strength(struct device_node *np)
{
	int ret;
	u32 val;

	ret = of_property_read_u32(np, "nand-ecc-strength", &val);
	return ret ? ret : val;
}

static inline bool of_get_nand_ecc_maximize(struct device_node *np)
{
	return of_property_read_bool(np, "nand-ecc-maximize");
}

void nand_ecc_read_user_conf(struct nand_device *nand)
{
	struct device_node *dn = nanddev_get_flash_node(nand);
	int strength, size;

	nand->ecc.user_conf.provider = of_get_nand_ecc_engine_type(dn);
	nand->ecc.user_conf.algo = of_get_nand_ecc_algo(dn);
	nand->ecc.user_conf.placement = of_get_nand_ecc_placement(dn);

	strength = of_get_nand_ecc_strength(dn);
	if (strength >= 0)
		nand->ecc.user_conf.strength = strength;

	size = of_get_nand_ecc_step_size(dn);
	if (size >= 0)
		nand->ecc.user_conf.step_size = size;

	if (of_get_nand_ecc_maximize(dn))
		nand->ecc.user_conf.flags |= NAND_ECC_MAXIMIZE;
}
EXPORT_SYMBOL(nand_ecc_read_user_conf);

/**
 * nand_ecc_correction_is_enough - Check if the chip configuration meets the
 *                                 datasheet requirements.
 *
 * @nand: Device to check
 *
 * If our configuration corrects A bits per B bytes and the minimum
 * required correction level is X bits per Y bytes, then we must ensure
 * both of the following are true:
 *
 * (1) A / B >= X / Y
 * (2) A >= X
 *
 * Requirement (1) ensures we can correct for the required bitflip density.
 * Requirement (2) ensures we can correct even when all bitflips are clumped
 * in the same sector.
 */
bool nand_ecc_correction_is_enough(struct nand_device *nand)
{
	struct nand_ecc_props *reqs = &nand->ecc.requirements;
	struct nand_ecc_props *conf = &nand->ecc.ctx.conf;
	struct mtd_info *mtd = nanddev_to_mtd(nand);
	int corr, ds_corr;

	if (conf->step_size == 0 || reqs->step_size == 0)
		/* Not enough information */
		return true;

	/*
	 * We get the number of corrected bits per page to compare
	 * the correction density.
	 */
	corr = (mtd->writesize * conf->strength) / conf->step_size;
	ds_corr = (mtd->writesize * reqs->strength) / reqs->step_size;

	return corr >= ds_corr && conf->strength >= reqs->strength;
}
EXPORT_SYMBOL(nand_ecc_correction_is_enough);

int nand_ecc_register_hw_engine(struct nand_ecc_engine *engine)
{
	struct nand_ecc_engine *item;

	if (!engine)
		return -ENOTSUPP;

	/* Prevent multiple registrations of one engine */
	list_for_each_entry(item, &hw_engines, node)
		if (item == engine)
			return 0;

	mutex_lock(&hw_engines_mutex);
	list_add_tail(&engine->node, &hw_engines);
	mutex_unlock(&hw_engines_mutex);

	return 0;
}
EXPORT_SYMBOL(nand_ecc_register_hw_engine);

int nand_ecc_unregister_hw_engine(struct nand_ecc_engine *engine)
{
	if (!engine)
		return -ENOTSUPP;

	mutex_lock(&hw_engines_mutex);
	list_del(&engine->node);
	mutex_unlock(&hw_engines_mutex);

	return 0;
}
EXPORT_SYMBOL(nand_ecc_unregister_hw_engine);

struct nand_ecc_engine *nand_ecc_get_sw_engine(struct nand_device *nand)
{
	unsigned int algo = nand->ecc.user_conf.algo;

	if (algo == NAND_ECC_UNKNOWN)
		algo = nand->ecc.defaults.algo;

	switch (algo) {
	case NAND_ECC_HAMMING:
		return nand_ecc_sw_hamming_get_engine();
	case NAND_ECC_BCH:
		return nand_ecc_sw_bch_get_engine();
	default:
		break;
	}

	return NULL;
}
EXPORT_SYMBOL(nand_ecc_get_sw_engine);

struct nand_ecc_engine *nand_ecc_get_ondie_engine(struct nand_device *nand)
{
	return nand->ecc.ondie_engine;
}
EXPORT_SYMBOL(nand_ecc_get_ondie_engine);

struct nand_ecc_engine *nand_ecc_match_hw_engine(struct device *dev)
{
	struct nand_ecc_engine *item;

	list_for_each_entry(item, &hw_engines, node)
		if (item->dev == dev)
			return item;

	return NULL;
}
EXPORT_SYMBOL(nand_ecc_match_hw_engine);

struct nand_ecc_engine *nand_ecc_get_hw_engine(struct nand_device *nand)
{
	struct nand_ecc_engine *engine = NULL;
	struct device *dev = &nand->mtd.dev;
	struct platform_device *pdev;
	struct device_node *np;

	if (list_empty(&hw_engines))
		return NULL;

	/* Check for an explicit ecc-engine property in the parent */
	np = of_parse_phandle(dev->of_node->parent, "ecc-engine", 0);
	if (np) {

		pdev = of_find_device_by_node(np);
		if (!pdev)
			return ERR_PTR(-EPROBE_DEFER);

		engine = nand_ecc_match_hw_engine(&pdev->dev);
		of_dev_put(pdev);
		of_node_put(np);
	}

	/* Support DTs without ecc-engine property: check the parent node */
	if (!engine) {
		pdev = of_find_device_by_node(dev->of_node->parent);
		if (pdev) {
			engine = nand_ecc_match_hw_engine(&pdev->dev);
			of_dev_put(pdev);
		}
	}

	/* Support no DT or very old DTs: check the node itself */
	if (!engine) {
		pdev = of_find_device_by_node(dev->of_node);
		if (pdev) {
			engine = nand_ecc_match_hw_engine(&pdev->dev);
			of_dev_put(pdev);
		}
	}

	if (engine)
		get_device(engine->dev);

	return engine;
}
EXPORT_SYMBOL(nand_ecc_get_hw_engine);

void nand_ecc_put_hw_engine(struct nand_device *nand)
{
	put_device(nand->ecc.engine->dev);
}
EXPORT_SYMBOL(nand_ecc_put_hw_engine);

/* ECC engine driver internal helpers */
int nand_ecc_init_req_tweaking(struct nand_ecc_req_tweak_ctx *ctx,
			       struct nand_device *nand)
{
	//todo: modularize the allocation!
	int sz = nanddev_page_size(nand) + nanddev_per_page_oobsize(nand) + 4*4;

	ctx->nand = nand;
	ctx->spare_databuf = kzalloc(sz, GFP_KERNEL);
	if (!ctx->spare_databuf)
		return -ENOMEM;

	ctx->spare_oobbuf = ctx->spare_databuf + nanddev_per_page_oobsize(nand);

	return 0;
}
EXPORT_SYMBOL_GPL(nand_ecc_init_req_tweaking);

void nand_ecc_cleanup_req_tweaking(struct nand_ecc_req_tweak_ctx *ctx)
{
	kfree(ctx->spare_databuf);
	kfree(ctx->spare_oobbuf);
}
EXPORT_SYMBOL_GPL(nand_ecc_cleanup_req_tweaking);

/*
 * Ensure data and OOB area is fully read/written otherwise the correction might
 * not work as expected.
 */
void nand_ecc_tweak_req(struct nand_ecc_req_tweak_ctx *ctx,
			struct nand_page_io_req *req)
{
	struct nand_device *nand = ctx->nand;
	struct nand_page_io_req *orig, *tweak;

	/* Save the original request */
	ctx->orig_req = *req;
	ctx->bounce_data = false;
	ctx->bounce_oob = false;
	orig = &ctx->orig_req;
	tweak = req;

	/* Ensure the request covers the entire page */

	if (true) {//todo (orig->datalen != nanddev_page_size(nand)) {
		ctx->bounce_data = true;
		tweak->dataoffs = 0;
		tweak->datalen = nanddev_page_size(nand);
		tweak->databuf.in = ctx->spare_databuf;
		memset(tweak->databuf.in, 0xFF, nanddev_page_size(nand));
	}

	if (true) {//todoorig->ooblen != nanddev_per_page_oobsize(nand)) {
		ctx->bounce_oob = true;
		tweak->ooboffs = 0;
		tweak->ooblen = nanddev_per_page_oobsize(nand);
		tweak->oobbuf.in = ctx->spare_oobbuf;
		// todo modularize
		memset(tweak->oobbuf.in, 0xFF, nanddev_per_page_oobsize(nand) + 4 * 4);
	}

	/* Copy the data that must be writen in the bounce buffers, if needed */
	if (orig->type == NAND_PAGE_WRITE) {
		if (ctx->bounce_data)
			memcpy((void *)tweak->databuf.out + orig->dataoffs,
			       orig->databuf.out, orig->datalen);

		if (ctx->bounce_oob)
			memcpy((void *)tweak->oobbuf.out + orig->ooboffs,
			       orig->oobbuf.out, orig->ooblen);
	}
}
EXPORT_SYMBOL_GPL(nand_ecc_tweak_req);

void nand_ecc_restore_req(struct nand_ecc_req_tweak_ctx *ctx,
			  struct nand_page_io_req *req)
{
	struct nand_page_io_req *orig, *tweak;

	orig = &ctx->orig_req;
	tweak = req;

	/* Restore the data read from the bounce buffers, if needed */
	if (orig->type == NAND_PAGE_READ) {
		if (ctx->bounce_data)
			memcpy(orig->databuf.in,
			       tweak->databuf.in + orig->dataoffs,
			       orig->datalen);

		if (ctx->bounce_oob)
			memcpy(orig->oobbuf.in,
			       tweak->oobbuf.in + orig->ooboffs,
			       orig->ooblen);
	}

	/* Ensure the original request is restored */
	*req = *orig;
}
EXPORT_SYMBOL_GPL(nand_ecc_restore_req);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miquel Raynal <miquel.raynal@bootlin.com>");
MODULE_DESCRIPTION("Generic ECC engine");
