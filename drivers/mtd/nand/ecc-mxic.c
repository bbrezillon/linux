// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Macronix external hardware ECC engine for NAND devices, also
 * called DPE for Data Processing Engine.
 *
 * Copyright © 2019 Macronix
 * Author: Miquèl Raynal <miquel.raynal@bootlin.com>
 */

#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mtd/nand.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

/* DPE Configuration */
#define DP_CONFIG 0x00
#define   ECC_EN BIT(0)
#define   ECC_TYP_MASK GENMASK(6, 3)
#define   ECC_TYP(idx) ((idx << 3) & GENMASK(6, 3))
/* DPE Interrupt Status */
#define INTRPT_STS 0x04
#define   TRANS_CMPLT BIT(0)
#define   SDMA_MAIN BIT(1)
#define   SDMA_SPARE BIT(2)
#define   ECC_ERR BIT(3)
#define   TO_SPARE BIT(4)
#define   TO_MAIN BIT(5)
/* DPE Interrupt Status Enable */
#define INTRPT_STS_EN 0x08
/* DPE Interrupt Signal Enable */
#define INTRPT_SIG_EN 0x0C
/* Host Controller Configuration */
#define HC_CONFIG 0x10
#define   TRANS_TYP_DMA 0
#define   TRANS_TYP_IO BIT(4)
#define   LAYOUT_TYP_INTEGRATED 0
#define   LAYOUT_TYP_DISTRIBUTED BIT(2)
#define   BURST_TYP_FIXED 0
#define   BURST_TYP_INCREASING BIT(0)
/* Host Controller Slave Address */
#define HC_SLV_ADDR 0x14
/* ECC Chunk Size */
#define CHUNK_SIZE 0x20
/* Main Data Size */
#define MAIN_SIZE 0x24
/* Spare Data Size */
#define SPARE_SIZE 0x28
/* ECC Chunk Count */
#define CHUNK_CNT 0x30
/* SDMA Control */
#define SDMA_CTRL 0x40
#define   WRITE_NAND 0
#define   READ_NAND BIT(1)
#define   CONT_NAND BIT(29)
#define   CONT_SYSM BIT(30)
#define   SDMA_STRT BIT(31)
/* SDMA Address of Main Data */
#define SDMA_MAIN_ADDR 0x44
/* SDMA Address of Spare Data */
#define SDMA_SPARE_ADDR 0x48
/* DPE Version Number */
#define DP_VER 0xD0
#define   DP_VER_OFFSET 16

/* Status bytes between each chunk of spare data */
#define FREE_BYTES 10
#define ECC_BYTES 14
#define RSVD_BYTES 8
#define STAT_BYTES 4
#define   NO_ERR 0x00
#define   MAX_CORR_ERR 0x28
#define   UNCORR_ERR 0xFE
#define   ERASED_CHUNK 0xFF

/**
 * struct mxic_ecc_drvdata
 * @external: a pipelined engine will use DMA to retrieve the data and
 *             {compute the checksums,correct the data} on the fly while an
 *             external engine will rely on the bus controller to retrieve the
 *             data and will act on a buffer (3 times more traffic on the AXI).
 * @bus_ctrl_axi_slave_region: Memory region where the bus controller can be
 *                             managed as an AXI slave.
 */
struct mxic_ecc_drvdata {
	bool external;
	unsigned int bus_ctrl_axi_slave_region;
};

struct mxic_ecc_engine {
	struct device *dev;
	const struct mxic_ecc_drvdata *drvdata;
	void __iomem *regs;

	/* ECC machinery */
	unsigned int data_step_sz;
	unsigned int oob_step_sz;
	u8 *status;
	int steps;
	bool enabled;

	/* Completion boilerplate */
	int irq;
	struct completion complete;

	/* DMA boilerplate */
	struct nand_ecc_req_tweak_ctx req_ctx;
	u8 *oobwithstat;
	struct scatterlist sg[2];
	struct nand_page_io_req *req;
};

static int mxic_ecc_ooblayout_ecc(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct mxic_ecc_engine *eng = nand->ecc.ctx.priv;

	if (section < 0 || section >= eng->steps)
		return -ERANGE;

	oobregion->offset = (section * eng->oob_step_sz) + FREE_BYTES;
	oobregion->length = ECC_BYTES;

	return 0;
}

static int mxic_ecc_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct mxic_ecc_engine *eng = nand->ecc.ctx.priv;

	if (section < 0 || section >= eng->steps)
		return -ERANGE;

	if (!section) {
		oobregion->offset = 2;
		oobregion->length = FREE_BYTES - 2;
	} else {
		oobregion->offset = section * eng->oob_step_sz;
		oobregion->length = FREE_BYTES;
	}

	return 0;
}

static const struct mtd_ooblayout_ops mxic_ecc_ooblayout_ops = {
	.ecc = mxic_ecc_ooblayout_ecc,
	.free = mxic_ecc_ooblayout_free,
};

static void mxic_ecc_disable_engine(struct mxic_ecc_engine *eng)
{
	u32 reg;

	reg = readl(eng->regs + DP_CONFIG);
	reg &= ~ECC_EN;
	writel(reg, eng->regs + DP_CONFIG);

	eng->enabled = false;
}

static void mxic_ecc_enable_engine(struct mxic_ecc_engine *eng)
{
	u32 reg;

	reg = readl(eng->regs + DP_CONFIG);
	reg |= ECC_EN;
	writel(reg, eng->regs + DP_CONFIG);

	eng->enabled = true;
}

static void mxic_ecc_disable_int(struct mxic_ecc_engine *eng)
{
	writel(0, eng->regs + INTRPT_SIG_EN);
}

static void mxic_ecc_enable_int(struct mxic_ecc_engine *eng)
{
	writel(TRANS_CMPLT, eng->regs + INTRPT_SIG_EN);
}

static irqreturn_t mxic_ecc_isr(int irq, void *dev_id)
{
	struct mxic_ecc_engine *eng = dev_id;
	u32 sts;

	sts = readl(eng->regs + INTRPT_STS);
	if (!sts)
		return IRQ_NONE;

	if (sts & TRANS_CMPLT)
		complete(&eng->complete);

	writel(sts, eng->regs + INTRPT_STS);

	return IRQ_HANDLED;
}

static int mxic_ecc_init_ctx(struct nand_device *nand)
{
	struct device *dev = nand->ecc.engine->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct nand_ecc_props *conf = &nand->ecc.ctx.conf;
	struct nand_ecc_props *reqs = &nand->ecc.requirements;
	struct nand_ecc_props *user = &nand->ecc.user_conf;
	struct mtd_info *mtd = nanddev_to_mtd(nand);
	struct mxic_ecc_engine *eng;
	int step_size = 0, strength = 0, desired_correction = 0, steps, idx;
	int possible_strength[] = {4, 8, 40, 48};
	int spare_size[] = {32, 32, 96, 96};
	int ret;

	eng = devm_kzalloc(dev, sizeof(*eng), GFP_KERNEL);
	if (!eng)
		return -ENOMEM;

	nand->ecc.ctx.priv = eng;
	nand->ecc.engine->priv = eng;

	eng->dev = dev;
	eng->drvdata = of_device_get_match_data(dev);

	/*
	 * Both memory regions for the ECC engine itself and the AXI slave
	 * address are mandatory.
	 */
	eng->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eng->regs)) {
		dev_err(dev, "Missing memory region\n");
		return PTR_ERR(eng->regs);
	}

	mxic_ecc_disable_engine(eng);
	mxic_ecc_disable_int(eng);

	/* Set the host controller AXI slave address for DMA access */
	writel(eng->drvdata->bus_ctrl_axi_slave_region,
	       eng->regs + HC_SLV_ADDR);

	/* IRQ is optional yet much more efficient */
	eng->irq = platform_get_irq_byname(pdev, "ecc-engine");
	if (eng->irq > 0) {
		ret = devm_request_irq(dev, eng->irq, mxic_ecc_isr, 0,
				       "mxic-ecc", eng);
		if (ret)
			return ret;
	} else {
		dev_info(dev, "No ECC engine IRQ (%d), using polling\n",
			 eng->irq);
		eng->irq = 0;
	}

	/* Only large page NAND chips may use BCH */
	if (mtd->oobsize < 64) {
		pr_err("BCH cannot be used with small page NAND chips\n");
		return -EINVAL;
	}

	mtd_set_ooblayout(mtd, &mxic_ecc_ooblayout_ops);

	/* Enable all status bits */
	writel(TRANS_CMPLT | SDMA_MAIN | SDMA_SPARE | ECC_ERR |
	       TO_SPARE | TO_MAIN, eng->regs + INTRPT_STS_EN);

	/* Configure the correction depending on the NAND device topology */
	if (user->step_size && user->strength) {
		step_size = user->step_size;
		strength = user->strength;
	} else if (reqs->step_size && reqs->strength) {
		step_size = reqs->step_size;
		strength = reqs->strength;
	}

	if (step_size && strength) {
		steps = mtd->writesize / step_size;
		desired_correction = steps * strength;
	}

	/* Step size is fixed to 1kiB, strength may vary (4 possible values) */
	conf->step_size = SZ_1K;
	steps = mtd->writesize / conf->step_size;

	eng->status = devm_kzalloc(dev, steps * sizeof(u8), GFP_KERNEL);
	if (!eng->status)
		return -ENOMEM;

	if (desired_correction) {
		strength = desired_correction / steps;

		for (idx = 0; idx < ARRAY_SIZE(possible_strength); idx++)
			if (possible_strength[idx] >= strength)
				break;

		idx = min_t(unsigned int, idx,
			    ARRAY_SIZE(possible_strength) - 1);
	} else {
		/* Missing data, maximize the correction */
		idx = ARRAY_SIZE(possible_strength) - 1;
	}

	/* Tune the selected strength until it fits in the OOB area */
	for (; idx >= 0; idx--) {
		if (spare_size[idx] * steps <= mtd->oobsize)
			break;
	}

	/* This engine cannot be used with this NAND device */
	if (idx < 0)
		return -EINVAL;

	/* Configure the engine for the desired strength */
	writel(ECC_TYP(idx), eng->regs + DP_CONFIG);
	conf->strength = possible_strength[idx];

	/*
	 * Trigger each step manually in external mode, while all steps should
	 * be handled in one go directly by the internal DMA in pipelined mode.
	 */
	if (eng->drvdata->external)
		writel(1, eng->regs + CHUNK_CNT);
	else
		writel(steps, eng->regs + CHUNK_CNT);

	eng->steps = steps;
	eng->data_step_sz = mtd->writesize / steps;
	eng->oob_step_sz = mtd->oobsize / steps;

	/*
	 * Use a syndrome layout in pipelined mode to reduce the complexity of
	 * the interaction between the ECC engine and the bus controller (also
	 * called 'distributed' in the spec) while a linear layout is much more
	 * easy to handle when in external ECC engine mode (also called
	 * 'integrated' in the spec)
	 */
	if (eng->drvdata->external)
		writel(BURST_TYP_INCREASING | LAYOUT_TYP_INTEGRATED |
		       TRANS_TYP_IO, eng->regs + HC_CONFIG);
	else
		writel(BURST_TYP_INCREASING | LAYOUT_TYP_DISTRIBUTED |
		       TRANS_TYP_DMA, eng->regs + HC_CONFIG);

	//todo: check return code
	//todo: cleanup in case of error
	nand_ecc_init_req_tweaking(&eng->req_ctx, nand);

	//todo: enlever le 4
	eng->oobwithstat = kmalloc(mtd->oobsize + 4*STAT_BYTES, GFP_KERNEL);
	if (!eng->oobwithstat)
		return -ENOMEM;

	sg_init_table(eng->sg, 2);

	/* Optional: check the registers are updated accordingly */
	dev_dbg(dev, "DPE version number: %d\n",
		readl(eng->regs + DP_VER) >> DP_VER_OFFSET);
	dev_dbg(dev, "Chunk count: %d\n", readl(eng->regs + CHUNK_CNT));
	dev_dbg(dev, "Chunk size: %d\n", readl(eng->regs + CHUNK_SIZE));
	dev_dbg(dev, "Main size: %d\n", readl(eng->regs + MAIN_SIZE));
	dev_dbg(dev, "Spare size: %d\n", readl(eng->regs + SPARE_SIZE) >> 24);
	dev_dbg(dev, "Rsv size: %ld\n",
		(readl(eng->regs + SPARE_SIZE) & GENMASK(23, 16)) >> 16);
	dev_dbg(dev, "Parity size: %ld\n",
		(readl(eng->regs + SPARE_SIZE) & GENMASK(15, 8)) >> 8);
	dev_dbg(dev, "Meta size: %ld\n",
		readl(eng->regs + SPARE_SIZE) & GENMASK(0, 7));

	return 0;
}

static void mxic_ecc_cleanup_ctx(struct nand_device *nand)
{
	struct mxic_ecc_engine *eng = nand->ecc.ctx.priv;

	if (eng) {
		//todo: clean req context
		kfree(eng->oobwithstat);
	}
}

static int mxic_ecc_data_xfer_wait_for_completion(struct mxic_ecc_engine *eng)
{
	u32 val;
	int ret;

	if (eng->irq) {
		init_completion(&eng->complete);
		mxic_ecc_enable_int(eng);
		ret = wait_for_completion_timeout(&eng->complete,
						  msecs_to_jiffies(1000));
		mxic_ecc_disable_int(eng);
	} else {
		ret = readl_poll_timeout(eng->regs + INTRPT_STS, val,
					 val & TRANS_CMPLT, 10, USEC_PER_SEC);
		writel(val, eng->regs + INTRPT_STS);
	}

	if (ret) {
		dev_err(eng->dev, "Timeout on data xfer completion\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int mxic_ecc_process_data(struct mxic_ecc_engine *eng)
{
	/* Retrieve the direction */
	unsigned int dir = (eng->req->type == NAND_PAGE_READ) ?
			   READ_NAND : WRITE_NAND;

	/* Start processing */
	writel(SDMA_STRT | dir, eng->regs + SDMA_CTRL);

	/* Wait for completion */
	return mxic_ecc_data_xfer_wait_for_completion(eng);
}

static void mxic_ecc_trim_status_bytes(struct mxic_ecc_engine *eng,
				       u8 *buf)
{
	int next_stat_pos;
	int step;

	/* Extract the ECC status */
	for (step = 0; step < eng->steps; step++) {
		next_stat_pos = eng->oob_step_sz +
				((STAT_BYTES + eng->oob_step_sz) * step);

		eng->status[step] = buf[next_stat_pos];
	}

	/* Reconstruct the OOB buffer linearly (without the ECC status bytes) */
	for (step = 1; step < eng->steps; step++)
		memcpy(buf + (step * eng->oob_step_sz),
		       buf + (step * (eng->oob_step_sz + STAT_BYTES)),
		       eng->oob_step_sz);
}

static int mxic_ecc_check_sum(struct mxic_ecc_engine *eng, struct mtd_info *mtd)
{
	struct device *dev = eng->dev;
	unsigned int max_bf = 0;
	int step;

	for (step = 0; step < eng->steps; step++) {
		u8 stat = eng->status[step];

		if (stat == NO_ERR) {
			dev_dbg(dev, "ECC step %d: no errror\n", step);
		} else if (stat == ERASED_CHUNK) {
			dev_dbg(dev, "ECC step %d: erased\n", step);
		} else if (stat == UNCORR_ERR || stat > MAX_CORR_ERR) {
			dev_dbg(dev, "ECC step %d: uncorr\n", step);
			mtd->ecc_stats.failed++;
		} else {
			dev_dbg(dev, "ECC step %d: %d corrected bf\n",
				step, stat);
			mtd->ecc_stats.corrected += stat;
			max_bf = max_t(unsigned int, max_bf, stat);
		}
	}

	return max_bf;
}

/* External ECC engine (linear layout) helpers */
static int mxic_ecc_prepare_io_req_external(struct nand_device *nand,
					    struct nand_page_io_req *req)
{
	struct mxic_ecc_engine *eng = nand->ecc.ctx.priv;
	int nents, step, ret;

	if (req->mode == MTD_OPS_RAW)
		return 0;

	nand_ecc_tweak_req(&eng->req_ctx, req);
	eng->req = req;

	if (req->type == NAND_PAGE_READ)
		return 0;

	sg_set_buf(&eng->sg[0], req->databuf.out, req->datalen);
	sg_set_buf(&eng->sg[1], req->oobbuf.out, req->ooblen);
	nents = dma_map_sg(eng->dev, eng->sg, 2, DMA_BIDIRECTIONAL);
	if (!nents)
		return -EINVAL;

	mxic_ecc_enable_engine(eng);

	for (step = 0; step < eng->steps; step++) {
		writel(sg_dma_address(&eng->sg[0]) + (step * eng->data_step_sz),
		       eng->regs + SDMA_MAIN_ADDR);
		writel(sg_dma_address(&eng->sg[1]) + (step * eng->oob_step_sz),
		       eng->regs + SDMA_SPARE_ADDR);
		ret = mxic_ecc_process_data(eng);
		if (ret)
			break;
	}

	mxic_ecc_disable_engine(eng);

	dma_unmap_sg(eng->dev, eng->sg, 2, DMA_BIDIRECTIONAL);

	return ret;
}

static int mxic_ecc_finish_io_req_external(struct nand_device *nand,
					   struct nand_page_io_req *req)
{
	struct mxic_ecc_engine *eng = nand->ecc.ctx.priv;
	struct mtd_info *mtd = nanddev_to_mtd(nand);
	int nents, step, ret;

	if (req->mode == MTD_OPS_RAW)
		return 0;

	if (req->type == NAND_PAGE_WRITE) {
		nand_ecc_restore_req(&eng->req_ctx, req);
		return 0;
	}

	/* Copy the OOB buffer and add room for the ECC engine status bytes */
	for (step = 0; step < eng->steps; step++)
		memcpy(eng->oobwithstat +
		       (step * (eng->oob_step_sz + STAT_BYTES)),
		       req->oobbuf.in +
		       (step * eng->oob_step_sz),
		       eng->oob_step_sz);

	sg_set_buf(&eng->sg[0], req->databuf.in, req->datalen);
	sg_set_buf(&eng->sg[1], eng->oobwithstat, req->ooblen +
						  (eng->steps * STAT_BYTES));
	nents = dma_map_sg(eng->dev, eng->sg, 2, DMA_BIDIRECTIONAL);
	if (!nents)
		return -EINVAL;

	mxic_ecc_enable_engine(eng);

	for (step = 0; step < eng->steps; step++) {
		writel(sg_dma_address(&eng->sg[0]) + (step * eng->data_step_sz),
		       eng->regs + SDMA_MAIN_ADDR);
		writel(sg_dma_address(&eng->sg[1]) +
		       (step * (eng->oob_step_sz + STAT_BYTES)),
		       eng->regs + SDMA_SPARE_ADDR);
		ret = mxic_ecc_process_data(eng);
		if (ret)
			break;
	}

	mxic_ecc_disable_engine(eng);

	dma_unmap_sg(eng->dev, eng->sg, 2, DMA_BIDIRECTIONAL);

	/* Trim the the 4 status bytes added by the ECC engine */
	mxic_ecc_trim_status_bytes(eng, eng->oobwithstat);

	nand_ecc_restore_req(&eng->req_ctx, req);

	return mxic_ecc_check_sum(eng, mtd);
}

static int mxic_ecc_deconstruct_raw_buffers(struct mxic_ecc_engine *eng)
{
	unsigned int chunk_sz = eng->data_step_sz + eng->oob_step_sz;
	u8 *data_src = eng->req->databuf.in;
	u8 *oob_src = eng->req->oobbuf.in;
	int step;
	u8 *tmp;

	/*
	 * memcpy() does not support overlapping areas, we must use an
	 * additional temporary buffer for the copy.
	 */
	tmp = kzalloc(chunk_sz * eng->steps, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	/*
	 * 1- Move the data with space between chunks (this works because we
	 * know that req->databuf.in and req->oobbuf.in are contiguous thanks to
	 * the bounce buffer).
	 */
	for (step = 0; step < eng->steps; step++)
		memcpy(tmp + (step * chunk_sz),
		       data_src + (step * eng->data_step_sz),
		       eng->data_step_sz);

	/* 2- Do the same with the OOB bytes */
	for (step = 0; step < eng->steps; step++)
		memcpy(tmp + eng->data_step_sz + (step * chunk_sz),
		       oob_src + (step * eng->oob_step_sz),
		       eng->oob_step_sz);

	/* 3- Re-copy the data back into the original buffer */
	memcpy(data_src, tmp, chunk_sz * eng->steps);
	kfree(tmp);

	return 0;
}

/* Pipelined ECC engine (distributed layout) helpers */
static int mxic_ecc_prepare_io_req_pipelined(struct nand_device *nand,
					     struct nand_page_io_req *req)
{
	struct mxic_ecc_engine *eng = nand->ecc.ctx.priv;
	int nents;

	nand_ecc_tweak_req(&eng->req_ctx, req);
	eng->req = req;

	if (req->mode == MTD_OPS_RAW && req->type == NAND_PAGE_WRITE)
		mxic_ecc_deconstruct_raw_buffers(eng);

	if (req->mode == MTD_OPS_RAW)
		return 0;

	if (req->type == NAND_PAGE_READ) {
		sg_set_buf(&eng->sg[0], req->databuf.in, req->datalen);
		sg_set_buf(&eng->sg[1], eng->oobwithstat,
			   req->ooblen + (eng->steps * STAT_BYTES));
	} else {
		sg_set_buf(&eng->sg[0], req->databuf.out, req->datalen);
		sg_set_buf(&eng->sg[1], req->oobbuf.out, req->ooblen);
	}
	nents = dma_map_sg(eng->dev, eng->sg, 2, DMA_BIDIRECTIONAL);
	if (!nents)
		return -EINVAL;

	writel(sg_dma_address(&eng->sg[0]), eng->regs + SDMA_MAIN_ADDR);
	writel(sg_dma_address(&eng->sg[1]), eng->regs + SDMA_SPARE_ADDR);

	mxic_ecc_enable_engine(eng);

	return 0;
}

static struct mxic_ecc_engine *host_dev_to_eng(struct device *host_dev)
{
	struct device_node *eng_node;
	struct platform_device *pdev;
	struct nand_ecc_engine *ecceng;

	eng_node = of_parse_phandle(host_dev->of_node, "ecc-engine", 0);
	pdev = eng_node ? of_find_device_by_node(eng_node) : NULL;
	ecceng = pdev ? nand_ecc_match_hw_engine(&pdev->dev) : NULL;

	return ecceng ? ecceng->priv : NULL;
}

bool mxic_ecc_use_engine(struct device *host_dev)
{
	struct mxic_ecc_engine *eng = host_dev_to_eng(host_dev);

	return eng ? eng->enabled : false;
}

int mxic_ecc_data_xfer(struct device *host_dev)
{
	struct mxic_ecc_engine *eng = host_dev_to_eng(host_dev);

	return mxic_ecc_process_data(eng);
}

static int mxic_ecc_reconstruct_raw_buffers(struct mxic_ecc_engine *eng,
					    bool has_stat_bytes)
{
	u8 *data_src = eng->req->databuf.in;
	unsigned int data_sz = eng->data_step_sz * eng->steps;
	unsigned int oob_step_sz = eng->oob_step_sz;
	unsigned int chunk_sz, tmp_sz;
	int step;
	u8 *tmp;
	int i;

	if (has_stat_bytes)
		oob_step_sz += STAT_BYTES;

	chunk_sz = eng->data_step_sz + oob_step_sz;
	tmp_sz = chunk_sz * eng->steps;

	/*
	 * memcpy() does not support overlapping areas, we must use an
	 * additional temporary buffer for the copy.
	 */
	tmp = kzalloc(tmp_sz, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	printk("dump raw buf:\n");
	for (i = 0; i < tmp_sz; i++)
		printk(KERN_CONT "%02x ", data_src[i]);
	printk(KERN_CONT "\n");

	/*
	 * In raw mode, data and OOB are mixed with a syndrome layout across the
	 * data and OOB buffer, reconstruct the data for the end user by:
	 * 1- Rebuilding the data area
	 */
	for (step = 0; step < eng->steps; step++)
		memcpy(tmp + (eng->data_step_sz * step),
		       data_src + (chunk_sz * step),
		       eng->data_step_sz);

	printk("dump tmp buf with data:\n");
	for (i = 0; i < tmp_sz; i++)
		printk(KERN_CONT "%02x ", tmp[i]);
	printk(KERN_CONT "\n");

	/* 2- Rebuilding the OOB area */
	for (step = 0; step < eng->steps; step++)
		memcpy(tmp + data_sz + (oob_step_sz * step),
		       data_src + eng->data_step_sz + (chunk_sz * step),
		       oob_step_sz);

	printk("dump tmp buf with oob:\n");
	for (i = 0; i < tmp_sz; i++)
		printk(KERN_CONT "%02x ", tmp[i]);
	printk(KERN_CONT "\n");

	//todo: allocate tmp buffer only once */
	/* 3- Copying back the tmp buffer in the original buffer */
	memcpy(data_src, tmp, tmp_sz);
	kfree(tmp);

	printk("dump final buf:\n");
	for (i = 0; i < tmp_sz; i++)
		printk(KERN_CONT "%02x ", data_src[i]);
	printk(KERN_CONT "\n");

	return 0;
}

static int mxic_ecc_finish_io_req_pipelined(struct nand_device *nand,
					    struct nand_page_io_req *req)
{
	struct mxic_ecc_engine *eng = nand->ecc.ctx.priv;
	struct mtd_info *mtd = nanddev_to_mtd(nand);
	int ret = 0;

	mxic_ecc_disable_engine(eng);

	dma_unmap_sg(eng->dev, eng->sg, 2, DMA_BIDIRECTIONAL);

	if (req->type == NAND_PAGE_READ) {
		if (req->mode == MTD_OPS_RAW) {
			ret = mxic_ecc_reconstruct_raw_buffers(eng, false);
		} else {
			mxic_ecc_reconstruct_raw_buffers(eng, true);
			mxic_ecc_trim_status_bytes(eng, eng->req->oobbuf.in);
			ret = mxic_ecc_check_sum(eng, mtd);
		}
	}

	nand_ecc_restore_req(&eng->req_ctx, req);

	return ret;
}

static struct nand_ecc_engine_ops mxic_ecc_engine_external_ops = {
	.init_ctx = mxic_ecc_init_ctx,
	.cleanup_ctx = mxic_ecc_cleanup_ctx,
	.prepare_io_req = mxic_ecc_prepare_io_req_external,
	.finish_io_req = mxic_ecc_finish_io_req_external,
};

static struct nand_ecc_engine_ops mxic_ecc_engine_pipelined_ops = {
	.init_ctx = mxic_ecc_init_ctx,
	.cleanup_ctx = mxic_ecc_cleanup_ctx,
	.prepare_io_req = mxic_ecc_prepare_io_req_pipelined,
	.finish_io_req = mxic_ecc_finish_io_req_pipelined,
};

static int mxic_ecc_probe(struct platform_device *pdev)
{
	const struct mxic_ecc_drvdata *d = of_device_get_match_data(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct nand_ecc_engine *ecceng;

	if (!d) {
		dev_err(dev, "Could not retrieve ECC data\n");
		return -EINVAL;
	}

	ecceng = devm_kzalloc(dev, sizeof(*ecceng), GFP_KERNEL);
	if (!ecceng)
		return -ENOMEM;

	ecceng->dev = dev;

	if (d->external)
		ecceng->ops = &mxic_ecc_engine_external_ops;
	else
		ecceng->ops = &mxic_ecc_engine_pipelined_ops;

	nand_ecc_register_hw_engine(ecceng);

	return 0;
}

static int mxic_ecc_remove(struct platform_device *pdev)
{
	struct nand_ecc_engine *ecceng = nand_ecc_match_hw_engine(&pdev->dev);

	if (ecceng)
		nand_ecc_unregister_hw_engine(ecceng);

	return 0;
}

static const struct mxic_ecc_drvdata mxic_ecc_spi_external_data = {
	.external = true,
	.bus_ctrl_axi_slave_region = 0xA0000000,
};

static const struct mxic_ecc_drvdata mxic_ecc_spi_pipelined_data = {
	.external = false,
	.bus_ctrl_axi_slave_region = 0xA0000000,
};

static const struct of_device_id mxic_ecc_of_ids[] = {
	{
		.compatible = "mxic,spi-external-ecc-engine",
		.data = &mxic_ecc_spi_external_data,
	},
	{
		.compatible = "mxic,spi-pipelined-ecc-engine",
		.data = &mxic_ecc_spi_pipelined_data,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mxic_ecc_of_ids);

static struct platform_driver mxic_ecc_ext_driver = {
	.driver	= {
		.name = "mxic-ecc-ext",
		.of_match_table = mxic_ecc_of_ids,
	},
	.probe = mxic_ecc_probe,
	.remove	= mxic_ecc_remove,
};
module_platform_driver(mxic_ecc_ext_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miquèl Raynal <miquel.raynal@bootlin.com>");
MODULE_DESCRIPTION("Macronix NAND hardware external ECC support");
