// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for One Laptop Per Child ‘CAFÉ’ controller, aka Marvell 88ALP01
 *
 * The data sheet for this device can be found at:
 *    http://wiki.laptop.org/go/Datasheets 
 *
 * Copyright © 2006 Red Hat, Inc.
 * Copyright © 2006 David Woodhouse <dwmw2@infradead.org>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>
#include <linux/rslib.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/iopoll.h>

#define CAFE_NAND_CTRL1				0x00
#define CAFE_NAND_CTRL1_HAS_CMD			BIT(31)
#define CAFE_NAND_CTRL1_HAS_ADDR		BIT(30)
#define CAFE_NAND_CTRL1_NUM_ADDR_CYC		GENMASK(29, 27)
#define CAFE_NAND_CTRL1_HAS_DATA_IN		BIT(26)
#define CAFE_NAND_CTRL1_HAS_DATA_OUT		BIT(25)
#define CAFE_NAND_CTRL1_NUM_NONMEM_READ_HIGH	GENMASK(24, 22)
#define CAFE_NAND_CTRL1_WAIT_BSY_AFTER_SEQ	BIT(21)
#define CAFE_NAND_CTRL1_NUM_NONMEM_READ_LOW	BIT(20)
#define CAFE_NAND_CTRL1_CE			BIT(19)
#define CAFE_NAND_CTRL1_CMD			GENMASK(7, 0)

#define CAFE_NAND_CTRL2				0x04
#define CAFE_NAND_CTRL2_AUTO_WRITE_ECC		BIT(30)
#define CAFE_NAND_CTRL2_PAGE_SIZE		GENMASK(29, 28)
#define CAFE_NAND_CTRL2_ECC_ALG_RS		BIT(27)
#define CAFE_NAND_CTRL2_HAS_CMD2		BIT(8)
#define CAFE_NAND_CTRL2_CMD2			GENMASK(7, 0)

#define CAFE_NAND_CTRL3				0x08
#define CAFE_NAND_CTRL3_READ_BUSY_RESET		BIT(31)
#define CAFE_NAND_CTRL3_WP			BIT(30)

#define CAFE_NAND_STATUS			0x0c
#define CAFE_NAND_STATUS_CONTROLLER_BUSY	BIT(31)
#define CAFE_NAND_STATUS_FLASH_BUSY		BIT(30)

#define CAFE_NAND_IRQ				0x10
#define CAFE_NAND_IRQ_MASK			0x14
#define CAFE_NAND_IRQ_CMD_DONE			BIT(31)
#define CAFE_NAND_IRQ_FLASH_RDY			BIT(30)
#define CAFE_NAND_IRQ_DMA_DONE			BIT(28)
#define CAFE_NAND_IRQ_BOOT_DONE			BIT(27)

#define CAFE_NAND_DATA_LEN			0x18
#define CAFE_NAND_ADDR1				0x1c
#define CAFE_NAND_ADDR2				0x20

#define CAFE_NAND_TIMING1			0x24
#define CAFE_NAND_TIMING1_TCLS			GENMASK(31, 28)
#define CAFE_NAND_TIMING1_TCLH			GENMASK(27, 24)
#define CAFE_NAND_TIMING1_TALS			GENMASK(23, 20)
#define CAFE_NAND_TIMING1_TALH			GENMASK(19, 16)
#define CAFE_NAND_TIMING1_TWB			GENMASK(15, 8)
#define CAFE_NAND_TIMING1_TRB			GENMASK(7, 0)

#define CAFE_NAND_TIMING2			0x28
#define CAFE_NAND_TIMING2_TRR			GENMASK(31, 28)
#define CAFE_NAND_TIMING2_TREA			GENMASK(27, 24)
#define CAFE_NAND_TIMING2_TDH			GENMASK(23, 20)
#define CAFE_NAND_TIMING2_TDS			GENMASK(19, 16)
#define CAFE_NAND_TIMING2_TRH			GENMASK(15, 12)
#define CAFE_NAND_TIMING2_TRP			GENMASK(11, 8)
#define CAFE_NAND_TIMING2_TWH			GENMASK(7, 4)
#define CAFE_NAND_TIMING2_TWP			GENMASK(3, 0)

#define CAFE_NAND_TIMING3			0x2c
#define CAFE_NAND_TIMING3_TAR			GENMASK(31, 28)
#define CAFE_NAND_TIMING3_TCLR			GENMASK(27, 24)

#define CAFE_NAND_NONMEM_READ_DATA		0x30
#define CAFE_NAND_ECC_READ_CODE			0x38

#define CAFE_NAND_ECC_RESULT			0x3C
#define CAFE_NAND_ECC_RESULT_RS_ERRORS		BIT(18)
#define CAFE_NAND_ECC_RESULT_STATUS		GENMASK(17, 16)
#define CAFE_NAND_ECC_RESULT_NO_ERROR		(0 << 16)
#define CAFE_NAND_ECC_RESULT_CORRECTABLE_ERRS	(1 << 16)
#define CAFE_NAND_ECC_RESULT_UNCORRECTABLE_ERRS	(2 << 16)
#define CAFE_NAND_ECC_RESULT_FAIL_BIT_LOC	GENMASK(13, 0)

#define CAFE_NAND_DMA_CTRL			0x40
#define CAFE_NAND_DMA_CTRL_ENABLE		BIT(31)
#define CAFE_NAND_DMA_CTRL_RESERVED		BIT(30)
#define CAFE_NAND_DMA_CTRL_DATA_IN		BIT(29)
#define CAFE_NAND_DMA_CTRL_DATA_LEN		GENMASK(11, 0)

#define CAFE_NAND_DMA_ADDR0			0x44
#define CAFE_NAND_DMA_ADDR1			0x48
#define CAFE_NAND_ECC_SYN_REG(x)		((((x) / 2) * 4) + 0x50)
#define CAFE_NAND_ECC_SYN_FIELD(x)		(((x) % 2) ? GENMASK(31, 16) : GENMASK(15, 0))

#define CAFE_NAND_CTRL4				0x60
#define CAFE_NAND_CTRL4_NO_READ_DELAY		BIT(8)

#define CAFE_NAND_DRIVE_STRENGTH		0x64
#define CAFE_NAND_DRIVE_STRENGTH_VAL		GENMASK(4, 0)

#define CAFE_NAND_READ_DATA			0x1000
#define CAFE_NAND_WRITE_DATA			0x2000

#define CAFE_GLOBAL_CTRL			0x3004
#define CAFE_GLOBAL_CCIC_CLK_ENABLE		BIT(14)
#define CAFE_GLOBAL_SDH_CLK_ENABLE		BIT(13)
#define CAFE_GLOBAL_NAND_CLK_ENABLE		BIT(12)
#define CAFE_GLOBAL_CLKRUN_ENABLE_SET		BIT(11)
#define CAFE_GLOBAL_CLKRUN_ENABLE_CLEAR		BIT(10)
#define CAFE_GLOBAL_SW_IRQ_SET			BIT(7)
#define CAFE_GLOBAL_SW_IRQ_CLEAR		BIT(6)
#define CAFE_GLOBAL_STOP_MASTER_DONE		BIT(5)
#define CAFE_GLOBAL_STOP_MASTER			BIT(4)
#define CAFE_GLOBAL_MASTER_RESET_CLEAR		BIT(3)
#define CAFE_GLOBAL_MASTER_RESET_SET		BIT(2)
#define CAFE_GLOBAL_SW_RESET_CLEAR		BIT(1)
#define CAFE_GLOBAL_SW_RESET_SET		BIT(0)

#define CAFE_GLOBAL_IRQ				0x3008
#define CAFE_GLOBAL_IRQ_MASK			0x300c
#define CAFE_GLOBAL_IRQ_PCI_ERROR		BIT(31)
#define CAFE_GLOBAL_IRQ_VPD_TWSI		BIT(26)
#define CAFE_GLOBAL_IRQ_CCIC			BIT(2)
#define CAFE_GLOBAL_IRQ_SDH			BIT(1)
#define CAFE_GLOBAL_IRQ_NAND			BIT(0)

#define CAFE_GLOBAL_RESET			0x3034
#define CAFE_GLOBAL_RESET_CCIC			BIT(2)
#define CAFE_GLOBAL_RESET_SDH			BIT(1)
#define CAFE_GLOBAL_RESET_NAND			BIT(0)

#define CAFE_FIELD_PREP(reg, field, val)	FIELD_PREP(CAFE_##reg##_##field, val)
#define CAFE_FIELD_GET(reg, field, val)		FIELD_GET(CAFE_##reg##_##field, val)

struct cafe_priv {
	struct nand_controller base;
	struct nand_chip nand;
	struct pci_dev *pdev;
	void __iomem *mmio;
	struct rs_control *rs;
	u32 ctl2;
	bool usedma;
	dma_addr_t dmaaddr;
	unsigned char *dmabuf;
};

static int usedma = 1;
module_param(usedma, int, 0644);

static int skipbbt = 0;
module_param(skipbbt, int, 0644);

static int regdebug = 0;
module_param(regdebug, int, 0644);

static int checkecc = 1;
module_param(checkecc, int, 0644);

static unsigned int numtimings;
static int timing[3];
module_param_array(timing, int, &numtimings, 0644);

static const char *part_probes[] = { "cmdlinepart", "RedBoot", NULL };

static int cafe_nand_write_oob(struct nand_chip *chip, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	return nand_prog_page_op(chip, page, mtd->writesize, chip->oob_poi,
				 mtd->oobsize);
}

/* Don't use -- use nand_read_oob_std for now */
static int cafe_nand_read_oob(struct nand_chip *chip, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	return nand_read_oob_op(chip, page, 0, chip->oob_poi, mtd->oobsize);
}

/*
 * The hw generator calculates the error syndrome automatically. Therefore
 * we need a special oob layout and handling.
 */
static int cafe_nand_read_page(struct nand_chip *chip, u8 *buf,
			       int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	void *pagebuf = nand_get_data_buf(chip);
	unsigned int max_bitflips = 0;
	u32 ecc_result;

	dev_dbg(&cafe->pdev->dev, "ECC result %08x SYN1,2 %08x\n",
		readl(cafe->mmio + CAFE_NAND_ECC_RESULT),
		readl(cafe->mmio + CAFE_NAND_ECC_SYN_REG(0)));

	nand_read_page_op(chip, page, 0, pagebuf,
			  mtd->writesize + mtd->oobsize);

	if (buf != pagebuf)
		memcpy(buf, pagebuf, mtd->writesize);

	ecc_result = readl(cafe->mmio + CAFE_NAND_ECC_RESULT);
	if (checkecc && (ecc_result & CAFE_NAND_ECC_RESULT_RS_ERRORS)) {
		unsigned short syn[8], pat[4];
		int pos[4];
		u8 *oob = chip->oob_poi;
		int i, n;

		for (i=0; i<8; i+=2) {
			u32 tmp = readl(cafe->mmio + CAFE_NAND_ECC_SYN_REG(i));
			u16 idx;

			idx = FIELD_GET(CAFE_NAND_ECC_SYN_FIELD(i), tmp);
			syn[i] = cafe->rs->codec->index_of[idx];
			idx = FIELD_GET(CAFE_NAND_ECC_SYN_FIELD(i + 1), tmp);
			syn[i+1] = cafe->rs->codec->index_of[idx];
		}

		n = decode_rs16(cafe->rs, NULL, NULL, 1367, syn, 0, pos, 0,
				pat);

		for (i = 0; i < n; i++) {
			int p = pos[i];

			/* The 12-bit symbols are mapped to bytes here */

			if (p > 1374) {
				/* out of range */
				n = -1374;
			} else if (p == 0) {
				/* high four bits do not correspond to data */
				if (pat[i] > 0xff)
					n = -2048;
				else
					buf[0] ^= pat[i];
			} else if (p == 1365) {
				buf[2047] ^= pat[i] >> 4;
				oob[0] ^= pat[i] << 4;
			} else if (p > 1365) {
				if ((p & 1) == 1) {
					oob[3*p/2 - 2048] ^= pat[i] >> 4;
					oob[3*p/2 - 2047] ^= pat[i] << 4;
				} else {
					oob[3*p/2 - 2049] ^= pat[i] >> 8;
					oob[3*p/2 - 2048] ^= pat[i];
				}
			} else if ((p & 1) == 1) {
				buf[3*p/2] ^= pat[i] >> 4;
				buf[3*p/2 + 1] ^= pat[i] << 4;
			} else {
				buf[3*p/2 - 1] ^= pat[i] >> 8;
				buf[3*p/2] ^= pat[i];
			}
		}

		if (n < 0) {
			dev_dbg(&cafe->pdev->dev,
				"Failed to correct ECC at %08x\n",
				readl(cafe->mmio + CAFE_NAND_ADDR2) * 2048);
			for (i = 0; i < 0x5c; i += 4)
				printk("Register %x: %08x\n", i,
				       readl(cafe->mmio + i));
			mtd->ecc_stats.failed++;
		} else {
			dev_dbg(&cafe->pdev->dev, "Corrected %d symbol errors\n", n);
			mtd->ecc_stats.corrected += n;
			max_bitflips = max_t(unsigned int, max_bitflips, n);
		}
	}

	return max_bitflips;
}

static int cafe_ooblayout_ecc(struct mtd_info *mtd, int section,
			      struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section)
		return -ERANGE;

	oobregion->offset = 0;
	oobregion->length = chip->ecc.total;

	return 0;
}

static int cafe_ooblayout_free(struct mtd_info *mtd, int section,
			       struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section)
		return -ERANGE;

	oobregion->offset = chip->ecc.total;
	oobregion->length = mtd->oobsize - chip->ecc.total;

	return 0;
}

static const struct mtd_ooblayout_ops cafe_ooblayout_ops = {
	.ecc = cafe_ooblayout_ecc,
	.free = cafe_ooblayout_free,
};

/* Ick. The BBT code really ought to be able to work this bit out
   for itself from the above, at least for the 2KiB case */
static u8 cafe_bbt_pattern_2048[] = { 'B', 'b', 't', '0' };
static u8 cafe_mirror_pattern_2048[] = { '1', 't', 'b', 'B' };

static u8 cafe_bbt_pattern_512[] = { 0xBB };
static u8 cafe_mirror_pattern_512[] = { 0xBC };


static struct nand_bbt_descr cafe_bbt_main_descr_2048 = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION,
	.offs =	14,
	.len = 4,
	.veroffs = 18,
	.maxblocks = 4,
	.pattern = cafe_bbt_pattern_2048
};

static struct nand_bbt_descr cafe_bbt_mirror_descr_2048 = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION,
	.offs =	14,
	.len = 4,
	.veroffs = 18,
	.maxblocks = 4,
	.pattern = cafe_mirror_pattern_2048
};

static struct nand_bbt_descr cafe_bbt_main_descr_512 = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION,
	.offs =	14,
	.len = 1,
	.veroffs = 15,
	.maxblocks = 4,
	.pattern = cafe_bbt_pattern_512
};

static struct nand_bbt_descr cafe_bbt_mirror_descr_512 = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION,
	.offs =	14,
	.len = 1,
	.veroffs = 15,
	.maxblocks = 4,
	.pattern = cafe_mirror_pattern_512
};


static int cafe_nand_write_page(struct nand_chip *chip,
				const u8 *buf, int oob_required,
				int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	void *pagebuf = nand_get_data_buf(chip);
	int ret;

	if (pagebuf != buf)
		memcpy(pagebuf, buf, mtd->writesize);

	/* Set up ECC autogeneration */
	cafe->ctl2 |= CAFE_NAND_CTRL2_AUTO_WRITE_ECC;

	ret = nand_prog_page_op(chip, page, 0, pagebuf,
				mtd->writesize + mtd->oobsize);

	/*
	 * And clear it before returning so that following write operations
	 * that do not involve ECC don't generate ECC bytes.
	 */
	cafe->ctl2 &= ~CAFE_NAND_CTRL2_AUTO_WRITE_ECC;

	return ret;
}

/* F_2[X]/(X**6+X+1)  */
static unsigned short gf64_mul(u8 a, u8 b)
{
	u8 c;
	unsigned int i;

	c = 0;
	for (i = 0; i < 6; i++) {
		if (a & 1)
			c ^= b;
		a >>= 1;
		b <<= 1;
		if ((b & 0x40) != 0)
			b ^= 0x43;
	}

	return c;
}

/* F_64[X]/(X**2+X+A**-1) with A the generator of F_64[X]  */
static u16 gf4096_mul(u16 a, u16 b)
{
	u8 ah, al, bh, bl, ch, cl;

	ah = a >> 6;
	al = a & 0x3f;
	bh = b >> 6;
	bl = b & 0x3f;

	ch = gf64_mul(ah ^ al, bh ^ bl) ^ gf64_mul(al, bl);
	cl = gf64_mul(gf64_mul(ah, bh), 0x21) ^ gf64_mul(al, bl);

	return (ch << 6) ^ cl;
}

static int cafe_mul(int x)
{
	if (x == 0)
		return 1;
	return gf4096_mul(x, 0xe01);
}

static int cafe_nand_attach_chip(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	int err = 0;

	cafe->dmabuf = dma_alloc_coherent(&cafe->pdev->dev, 2112,
					  &cafe->dmaaddr, GFP_KERNEL);
	if (!cafe->dmabuf)
		return -ENOMEM;

	/* Set up DMA address */
	writel(lower_32_bits(cafe->dmaaddr), cafe->mmio + CAFE_NAND_DMA_ADDR0);
	writel(upper_32_bits(cafe->dmaaddr), cafe->mmio + CAFE_NAND_DMA_ADDR1);

	dev_dbg(&cafe->pdev->dev, "Set DMA address to %x (virt %p)\n",
		readl(cafe->mmio + CAFE_NAND_DMA_ADDR0), cafe->dmabuf);

	/* Restore the DMA flag */
	cafe->usedma = usedma;

	cafe->ctl2 = CAFE_NAND_CTRL2_ECC_ALG_RS |
		     CAFE_FIELD_PREP(NAND_CTRL2, PAGE_SIZE,
				     mtd->writesize / 1024);

	/* Set up ECC according to the type of chip we found */
	mtd_set_ooblayout(mtd, &cafe_ooblayout_ops);
	if (mtd->writesize == 2048) {
		cafe->nand.bbt_td = &cafe_bbt_main_descr_2048;
		cafe->nand.bbt_md = &cafe_bbt_mirror_descr_2048;
	} else if (mtd->writesize == 512) {
		cafe->nand.bbt_td = &cafe_bbt_main_descr_512;
		cafe->nand.bbt_md = &cafe_bbt_mirror_descr_512;
	} else {
		dev_warn(&cafe->pdev->dev,
			 "Unexpected NAND flash writesize %d. Aborting\n",
			 mtd->writesize);
		err = -ENOTSUPP;
		goto out_free_dma;
	}

	cafe->nand.ecc.mode = NAND_ECC_HW;
	cafe->nand.ecc.algo = NAND_ECC_RS;
	cafe->nand.ecc.size = mtd->writesize;
	cafe->nand.ecc.bytes = 14;
	cafe->nand.ecc.strength = 4;
	cafe->nand.ecc.write_page = cafe_nand_write_page;
	cafe->nand.ecc.write_oob = cafe_nand_write_oob;
	cafe->nand.ecc.read_page = cafe_nand_read_page;
	cafe->nand.ecc.read_oob = cafe_nand_read_oob;

	return 0;

 out_free_dma:
	dma_free_coherent(&cafe->pdev->dev, 2112, cafe->dmabuf, cafe->dmaaddr);

	return err;
}

static void cafe_nand_detach_chip(struct nand_chip *chip)
{
	struct cafe_priv *cafe = nand_get_controller_data(chip);

	dma_free_coherent(&cafe->pdev->dev, 2112, cafe->dmabuf, cafe->dmaaddr);
}

static void cafe_data_out(struct cafe_priv *cafe,
                          const void *buf, unsigned int len)
{
	if (cafe->usedma)
		memcpy(cafe->dmabuf, buf, len);
	else
		memcpy_toio(cafe->mmio + CAFE_NAND_WRITE_DATA, buf, len);
}

static void cafe_data_in(struct cafe_priv *cafe, bool non_mem_read,
                         void *buf, unsigned int len)
{
	if (non_mem_read) {
		u32 rd = readl(cafe->mmio + CAFE_NAND_NONMEM_READ_DATA);

		memcpy(buf, &rd, min_t(unsigned int, len, 4));
	} else if (cafe->usedma) {
		memcpy(buf, cafe->dmabuf, len);
	} else {
		memcpy_fromio(buf, cafe->mmio + CAFE_NAND_READ_DATA, len);
	}
}

static int cafe_nand_exec_subop(struct nand_chip *chip,
				const struct nand_subop *subop)
{
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	u32 ctrl1 = 0, ctrl2 = cafe->ctl2, addr1 = 0, addr2 = 0;
	u32 status, dmactrl = 0, wait = CAFE_NAND_IRQ_CMD_DONE;
	int ret, data_instr = -1;
	bool waitrdy = false;
	unsigned int i, j;

	ctrl1 |= CAFE_FIELD_PREP(NAND_CTRL1, CE, subop->cs);

	for (i = 0; i < subop->ninstrs; i++) {
		const struct nand_op_instr *instr = &subop->instrs[i];

		switch (instr->type) {
		case NAND_OP_CMD_INSTR:
			if (!(ctrl1 & CAFE_NAND_CTRL1_HAS_CMD))
				ctrl1 |= CAFE_NAND_CTRL1_HAS_CMD |
					 CAFE_FIELD_PREP(NAND_CTRL1, CMD,
							 instr->ctx.cmd.opcode);
			else
				ctrl2 |= CAFE_NAND_CTRL2_HAS_CMD2 |
					 CAFE_FIELD_PREP(NAND_CTRL2, CMD2,
							 instr->ctx.cmd.opcode);
			break;

		case NAND_OP_ADDR_INSTR:
			for (j = nand_subop_get_addr_start_off(subop, i);
			     j < nand_subop_get_num_addr_cyc(subop, i); j++) {
				u32 addr = instr->ctx.addr.addrs[j];

				if (j < 2)
					addr1 |= addr << (8 * j);
				else
					addr2 |= addr << (8 * (j - 2));
			}

			ctrl1 |= CAFE_NAND_CTRL1_HAS_ADDR |
				 CAFE_FIELD_PREP(NAND_CTRL1, NUM_ADDR_CYC,
						 instr->ctx.addr.naddrs - 1);
			writel(addr1, cafe->mmio + CAFE_NAND_ADDR1);
			writel(addr2, cafe->mmio + CAFE_NAND_ADDR2);
			break;

		case NAND_OP_DATA_IN_INSTR:
			data_instr = i;
			if (ctrl1 & CAFE_NAND_CTRL1_HAS_ADDR) {
				ctrl1 |= CAFE_NAND_CTRL1_HAS_DATA_IN;
			} else {
				ctrl1 |= CAFE_FIELD_PREP(NAND_CTRL1,
							 NUM_NONMEM_READ_LOW,
							 instr->ctx.data.len & 1) |
					 CAFE_FIELD_PREP(NAND_CTRL1,
							 NUM_NONMEM_READ_HIGH,
							 instr->ctx.data.len >> 1);
			}
			break;

		case NAND_OP_DATA_OUT_INSTR:
			data_instr = i;
			ctrl1 |= CAFE_NAND_CTRL1_HAS_DATA_OUT;
			break;

		case NAND_OP_WAITRDY_INSTR:
			wait |= CAFE_NAND_IRQ_FLASH_RDY;
			waitrdy = true;
			break;
		}
	}

	if (data_instr >= 0) {
		writel(nand_subop_get_data_len(subop, data_instr),
		       cafe->mmio + CAFE_NAND_DATA_LEN);
	}

	if (cafe->usedma && data_instr >= 0 &&
	    (ctrl1 & CAFE_NAND_CTRL1_HAS_ADDR)) {
		dmactrl |= CAFE_NAND_DMA_CTRL_ENABLE |
			   CAFE_NAND_DMA_CTRL_RESERVED |
			   CAFE_FIELD_PREP(NAND_DMA_CTRL, DATA_LEN,
					   nand_subop_get_data_len(subop,
								   data_instr));
		if (ctrl1 & CAFE_NAND_CTRL1_HAS_DATA_IN)
			dmactrl |= CAFE_NAND_DMA_CTRL_DATA_IN;

		/*
		 * If the last instruction is a data transfer and we're using
		 * DMA, we should wait on DMA_DONE only, otherwise, keep
		 * waiting on CMD_DONE.
		 */
		if (data_instr == subop->ninstrs - 1) {
			wait &= ~CAFE_NAND_IRQ_CMD_DONE;
			wait |= CAFE_NAND_IRQ_DMA_DONE;
		}
	}

	if (ctrl1 & CAFE_NAND_CTRL1_HAS_DATA_OUT) {
		cafe_data_out(cafe,
			      subop->instrs[data_instr].ctx.data.buf.out +
			      nand_subop_get_data_start_off(subop, data_instr),
			      nand_subop_get_data_len(subop, data_instr));
	}

	/* Clear pending interrupts before starting the operation. */
	writel(wait, cafe->mmio + CAFE_NAND_IRQ);

	writel(dmactrl, cafe->mmio + CAFE_NAND_DMA_CTRL);
	writel(ctrl2, cafe->mmio + CAFE_NAND_CTRL2);
	writel(ctrl1, cafe->mmio + CAFE_NAND_CTRL1);

	ret = readl_poll_timeout_atomic(cafe->mmio + CAFE_NAND_IRQ, status,
					(status & wait) == wait, 1,
					USEC_PER_SEC);
	if (ret)
		return ret;

	if (ctrl1 & CAFE_NAND_CTRL1_HAS_DATA_IN) {
		cafe_data_in(cafe, !!(ctrl1 & CAFE_NAND_CTRL1_HAS_ADDR),
			     subop->instrs[data_instr].ctx.data.buf.in +
			     nand_subop_get_data_start_off(subop, data_instr),
			     nand_subop_get_data_len(subop, data_instr));
	}

	return 0;
}

static const struct nand_op_parser cafe_nand_op_parser = NAND_OP_PARSER(
	NAND_OP_PARSER_PATTERN(cafe_nand_exec_subop,
			       NAND_OP_PARSER_PAT_CMD_ELEM(false),
			       NAND_OP_PARSER_PAT_ADDR_ELEM(false, 5),
			       NAND_OP_PARSER_PAT_CMD_ELEM(true),
			       NAND_OP_PARSER_PAT_WAITRDY_ELEM(true),
			       NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 2112)),
	NAND_OP_PARSER_PATTERN(cafe_nand_exec_subop,
			       NAND_OP_PARSER_PAT_CMD_ELEM(false),
			       NAND_OP_PARSER_PAT_ADDR_ELEM(false, 5),
			       NAND_OP_PARSER_PAT_DATA_OUT_ELEM(true, 2112),
			       NAND_OP_PARSER_PAT_CMD_ELEM(true),
			       NAND_OP_PARSER_PAT_WAITRDY_ELEM(true)),
	NAND_OP_PARSER_PATTERN(cafe_nand_exec_subop,
			       NAND_OP_PARSER_PAT_CMD_ELEM(false),
			       NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 4),
			       NAND_OP_PARSER_PAT_WAITRDY_ELEM(true))
);

static int cafe_nand_exec_op(struct nand_chip *chip,
			     const struct nand_operation *op,
			     bool check_only)
{
	return nand_op_parser_exec_op(chip, &cafe_nand_op_parser, op,
				      check_only);
}

static const struct nand_controller_ops cafe_nand_controller_ops = {
	.attach_chip = cafe_nand_attach_chip,
	.detach_chip = cafe_nand_detach_chip,
	.exec_op = cafe_nand_exec_op,
};

static void cafe_nand_init(struct cafe_priv *cafe)
{
	u32 ctrl;

	/* Start off by resetting the NAND controller completely */
	writel(CAFE_GLOBAL_RESET_NAND, cafe->mmio + CAFE_GLOBAL_RESET);
	writel(0, cafe->mmio + CAFE_GLOBAL_RESET);
	writel(0xffffffff, cafe->mmio + CAFE_NAND_IRQ_MASK);

	/* Restore timing configuration */
	writel(timing[0], cafe->mmio + CAFE_NAND_TIMING1);
	writel(timing[1], cafe->mmio + CAFE_NAND_TIMING2);
	writel(timing[2], cafe->mmio + CAFE_NAND_TIMING3);

	/* Disable master reset, enable NAND clock */
	ctrl = readl(cafe->mmio + CAFE_GLOBAL_CTRL);
	ctrl &= ~(CAFE_GLOBAL_SW_RESET_SET |
		  CAFE_GLOBAL_SW_RESET_CLEAR |
		  CAFE_GLOBAL_MASTER_RESET_SET |
		  CAFE_GLOBAL_MASTER_RESET_CLEAR |
		  CAFE_GLOBAL_NAND_CLK_ENABLE);
	ctrl |= CAFE_GLOBAL_NAND_CLK_ENABLE |
		CAFE_GLOBAL_SDH_CLK_ENABLE |
		CAFE_GLOBAL_CCIC_CLK_ENABLE;
	writel(ctrl | CAFE_GLOBAL_MASTER_RESET_SET | CAFE_GLOBAL_SW_RESET_SET,
	       cafe->mmio + CAFE_GLOBAL_CTRL);
	writel(ctrl | CAFE_GLOBAL_MASTER_RESET_CLEAR |
	       CAFE_GLOBAL_SW_RESET_CLEAR,
	       cafe->mmio + CAFE_GLOBAL_CTRL);

	writel(0, cafe->mmio + CAFE_NAND_DMA_CTRL);

	writel(CAFE_GLOBAL_NAND_CLK_ENABLE | CAFE_GLOBAL_SDH_CLK_ENABLE |
	       CAFE_GLOBAL_CCIC_CLK_ENABLE | CAFE_GLOBAL_MASTER_RESET_SET |
	       CAFE_GLOBAL_SW_RESET_CLEAR,
	       cafe->mmio + CAFE_GLOBAL_CTRL);
	writel(CAFE_GLOBAL_NAND_CLK_ENABLE | CAFE_GLOBAL_SDH_CLK_ENABLE |
	       CAFE_GLOBAL_CCIC_CLK_ENABLE | CAFE_GLOBAL_MASTER_RESET_CLEAR |
	       CAFE_GLOBAL_SW_RESET_CLEAR,
	       cafe->mmio + CAFE_GLOBAL_CTRL);

	/* Set up DMA address */
	writel(lower_32_bits(cafe->dmaaddr), cafe->mmio + CAFE_NAND_DMA_ADDR0);
	writel(upper_32_bits(cafe->dmaaddr), cafe->mmio + CAFE_NAND_DMA_ADDR1);
}

static int cafe_nand_probe(struct pci_dev *pdev,
				     const struct pci_device_id *ent)
{
	struct mtd_info *mtd;
	struct cafe_priv *cafe;
	int err = 0;

	/* Very old versions shared the same PCI ident for all three
	   functions on the chip. Verify the class too... */
	if ((pdev->class >> 8) != PCI_CLASS_MEMORY_FLASH)
		return -ENODEV;

	err = pci_enable_device(pdev);
	if (err)
		return err;

	pci_set_master(pdev);

	cafe = devm_kzalloc(&pdev->dev, sizeof(*cafe), GFP_KERNEL);
	if (!cafe)
		return  -ENOMEM;

	mtd = nand_to_mtd(&cafe->nand);
	mtd->dev.parent = &pdev->dev;
	nand_set_controller_data(&cafe->nand, cafe);

	cafe->pdev = pdev;
	cafe->mmio = pci_iomap(pdev, 0, 0);
	if (!cafe->mmio) {
		dev_warn(&pdev->dev, "failed to iomap\n");
		return -ENOMEM;
	}

	cafe->rs = init_rs_non_canonical(12, &cafe_mul, 0, 1, 8);
	if (!cafe->rs) {
		err = -ENOMEM;
		goto out_ior;
	}

	/* Enable the following for a flash based bad block table */
	cafe->nand.bbt_options = NAND_BBT_USE_FLASH;

	if (skipbbt)
		cafe->nand.options |= NAND_SKIP_BBTSCAN | NAND_NO_BBM_QUIRK;

	if (numtimings && numtimings != 3) {
		dev_warn(&cafe->pdev->dev, "%d timing register values ignored; precisely three are required\n", numtimings);
	}

	if (numtimings == 3) {
		dev_dbg(&cafe->pdev->dev, "Using provided timings (%08x %08x %08x)\n",
			timing[0], timing[1], timing[2]);
	} else {
		timing[0] = readl(cafe->mmio + CAFE_NAND_TIMING1);
		timing[1] = readl(cafe->mmio + CAFE_NAND_TIMING2);
		timing[2] = readl(cafe->mmio + CAFE_NAND_TIMING3);

		if (timing[0] | timing[1] | timing[2]) {
			dev_dbg(&cafe->pdev->dev, "Timing registers already set (%08x %08x %08x)\n",
				timing[0], timing[1], timing[2]);
		} else {
			dev_warn(&cafe->pdev->dev, "Timing registers unset; using most conservative defaults\n");
			timing[0] = timing[1] = timing[2] = 0xffffffff;
		}
	}

	cafe_nand_init(cafe);

	/* Do not use the DMA during the NAND identification */
	cafe->usedma = 0;

	/* Scan to find existence of the device */
	nand_controller_init(&cafe->base);
	cafe->base.ops = &cafe_nand_controller_ops;
	cafe->nand.controller = &cafe->base;
	err = nand_scan(&cafe->nand, 2);
	if (err)
		goto out_ior;

	pci_set_drvdata(pdev, mtd);

	mtd->name = "cafe_nand";
	err = mtd_device_parse_register(mtd, part_probes, NULL, NULL, 0);
	if (err)
		goto out_cleanup_nand;

	return 0;

 out_cleanup_nand:
	nand_cleanup(&cafe->nand);
 out_ior:
	pci_iounmap(pdev, cafe->mmio);
	return err;
}

static void cafe_nand_remove(struct pci_dev *pdev)
{
	struct mtd_info *mtd = pci_get_drvdata(pdev);
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	int ret;

	ret = mtd_device_unregister(mtd);
	WARN_ON(ret);
	nand_cleanup(chip);
	free_rs(cafe->rs);
	pci_iounmap(pdev, cafe->mmio);
	dma_free_coherent(&cafe->pdev->dev, 2112, cafe->dmabuf, cafe->dmaaddr);
}

static const struct pci_device_id cafe_nand_tbl[] = {
	{ PCI_VENDOR_ID_MARVELL, PCI_DEVICE_ID_MARVELL_88ALP01_NAND,
	  PCI_ANY_ID, PCI_ANY_ID },
	{ }
};

MODULE_DEVICE_TABLE(pci, cafe_nand_tbl);

static int cafe_nand_resume(struct pci_dev *pdev)
{
	struct mtd_info *mtd = pci_get_drvdata(pdev);
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct cafe_priv *cafe = nand_get_controller_data(chip);

	cafe_nand_init(cafe);

	return 0;
}

static struct pci_driver cafe_nand_pci_driver = {
	.name = "CAFÉ NAND",
	.id_table = cafe_nand_tbl,
	.probe = cafe_nand_probe,
	.remove = cafe_nand_remove,
	.resume = cafe_nand_resume,
};

module_pci_driver(cafe_nand_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_DESCRIPTION("NAND flash driver for OLPC CAFÉ chip");
