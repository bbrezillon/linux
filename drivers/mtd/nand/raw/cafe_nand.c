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
#include <linux/io.h>

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
	uint32_t ctl1;
	uint32_t ctl2;
	int datalen;
	int nr_data;
	int data_pos;
	int page_addr;
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

/* Make it easier to switch to PIO if we need to */
#define cafe_readl(cafe, addr)			readl((cafe)->mmio + CAFE_##addr)
#define cafe_writel(cafe, datum, addr)		writel(datum, (cafe)->mmio + CAFE_##addr)

static int cafe_device_ready(struct nand_chip *chip)
{
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	int result = !!(cafe_readl(cafe, NAND_STATUS) &
			CAFE_NAND_STATUS_FLASH_BUSY);
	uint32_t irqs = cafe_readl(cafe, NAND_IRQ);

	cafe_writel(cafe, irqs, NAND_IRQ);

	dev_dbg(&cafe->pdev->dev, "NAND device is%s ready, IRQ %x (%x) (%x,%x)\n",
		result?"":" not", irqs, cafe_readl(cafe, NAND_IRQ),
		cafe_readl(cafe, GLOBAL_IRQ), cafe_readl(cafe, GLOBAL_IRQ_MASK));

	return result;
}


static void cafe_write_buf(struct nand_chip *chip, const uint8_t *buf, int len)
{
	struct cafe_priv *cafe = nand_get_controller_data(chip);

	if (cafe->usedma)
		memcpy(cafe->dmabuf + cafe->datalen, buf, len);
	else
		memcpy_toio(cafe->mmio + CAFE_NAND_WRITE_DATA + cafe->datalen, buf, len);

	cafe->datalen += len;

	dev_dbg(&cafe->pdev->dev, "Copy 0x%x bytes to write buffer. datalen 0x%x\n",
		len, cafe->datalen);
}

static void cafe_read_buf(struct nand_chip *chip, uint8_t *buf, int len)
{
	struct cafe_priv *cafe = nand_get_controller_data(chip);

	if (cafe->usedma)
		memcpy(buf, cafe->dmabuf + cafe->datalen, len);
	else
		memcpy_fromio(buf, cafe->mmio + CAFE_NAND_READ_DATA + cafe->datalen, len);

	dev_dbg(&cafe->pdev->dev, "Copy 0x%x bytes from position 0x%x in read buffer.\n",
		len, cafe->datalen);
	cafe->datalen += len;
}

static uint8_t cafe_read_byte(struct nand_chip *chip)
{
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	uint8_t d;

	cafe_read_buf(chip, &d, 1);
	dev_dbg(&cafe->pdev->dev, "Read %02x\n", d);

	return d;
}

static void cafe_nand_cmdfunc(struct nand_chip *chip, unsigned command,
			      int column, int page_addr)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	int adrbytes = 0;
	uint32_t ctl1;
	uint32_t doneint = CAFE_NAND_IRQ_CMD_DONE;

	dev_dbg(&cafe->pdev->dev, "cmdfunc %02x, 0x%x, 0x%x\n",
		command, column, page_addr);

	if (command == NAND_CMD_ERASE2 || command == NAND_CMD_PAGEPROG) {
		/* Second half of a command we already calculated */
		cafe_writel(cafe,
			    cafe->ctl2 |
			    CAFE_NAND_CTRL2_HAS_CMD2 |
			    CAFE_FIELD_PREP(NAND_CTRL2, CMD2, command),
			    NAND_CTRL2);
		ctl1 = cafe->ctl1;
		dev_dbg(&cafe->pdev->dev, "Continue command, ctl1 %08x, #data %d\n",
			cafe->ctl1, cafe->nr_data);
		goto do_command;
	}
	/* Reset ECC engine */
	cafe_writel(cafe, 0, NAND_CTRL2);

	/* Emulate NAND_CMD_READOOB on large-page chips */
	if (mtd->writesize > 512 &&
	    command == NAND_CMD_READOOB) {
		column += mtd->writesize;
		command = NAND_CMD_READ0;
	}

	/* FIXME: Do we need to send read command before sending data
	   for small-page chips, to position the buffer correctly? */

	if (column != -1) {
		cafe_writel(cafe, column, NAND_ADDR1);
		adrbytes = 2;
		if (page_addr != -1)
			goto write_adr2;
	} else if (page_addr != -1) {
		cafe_writel(cafe, page_addr & 0xffff, NAND_ADDR1);
		page_addr >>= 16;
	write_adr2:
		cafe_writel(cafe, page_addr, NAND_ADDR2);
		adrbytes += 2;
		if (mtd->size > mtd->writesize << 16)
			adrbytes++;
	}

	cafe->data_pos = cafe->datalen = 0;

	/* Set command valid bit, mask in the chip select bit  */
	ctl1 = CAFE_NAND_CTRL1_HAS_CMD |
	       CAFE_FIELD_PREP(NAND_CTRL1, CMD, command) |
	       (cafe->ctl1 & CAFE_NAND_CTRL1_CE);

	/* Set RD or WR bits as appropriate */
	if (command == NAND_CMD_READID || command == NAND_CMD_STATUS) {
		ctl1 |= CAFE_NAND_CTRL1_HAS_DATA_IN;
		/* Always 5 bytes, for now */
		cafe->datalen = 4;
		/* And one address cycle -- even for STATUS, since the controller doesn't work without */
		adrbytes = 1;
	} else if (command == NAND_CMD_READ0 || command == NAND_CMD_READ1 ||
		   command == NAND_CMD_READOOB || command == NAND_CMD_RNDOUT) {
		ctl1 |= CAFE_NAND_CTRL1_HAS_DATA_IN;
		/* For now, assume just read to end of page */
		cafe->datalen = mtd->writesize + mtd->oobsize - column;
	} else if (command == NAND_CMD_SEQIN)
		ctl1 |= CAFE_NAND_CTRL1_HAS_DATA_OUT;

	/* Set number of address bytes */
	if (adrbytes)
		ctl1 |= CAFE_NAND_CTRL1_HAS_ADDR |
			CAFE_FIELD_PREP(NAND_CTRL1, NUM_ADDR_CYC, adrbytes - 1);

	if (command == NAND_CMD_SEQIN || command == NAND_CMD_ERASE1) {
		/* Ignore the first command of a pair; the hardware
		   deals with them both at once, later */
		cafe->ctl1 = ctl1;
		dev_dbg(&cafe->pdev->dev, "Setup for delayed command, ctl1 %08x, dlen %x\n",
			cafe->ctl1, cafe->datalen);
		return;
	}
	/* RNDOUT and READ0 commands need a following byte */
	if (command == NAND_CMD_RNDOUT)
		cafe_writel(cafe,
			    cafe->ctl2 | CAFE_NAND_CTRL2_HAS_CMD2 |
			    CAFE_FIELD_PREP(NAND_CTRL2, CMD2, NAND_CMD_RNDOUTSTART),
			    NAND_CTRL2);
	else if (command == NAND_CMD_READ0 && mtd->writesize > 512)
		cafe_writel(cafe,
			    cafe->ctl2 | CAFE_NAND_CTRL2_HAS_CMD2 |
			    CAFE_FIELD_PREP(NAND_CTRL2, CMD2, NAND_CMD_READSTART),
			    NAND_CTRL2);

 do_command:
	dev_dbg(&cafe->pdev->dev, "dlen %x, ctl1 %x, ctl2 %x\n",
		cafe->datalen, ctl1, cafe_readl(cafe, NAND_CTRL2));

	/* NB: The datasheet lies -- we really should be subtracting 1 here */
	cafe_writel(cafe, cafe->datalen, NAND_DATA_LEN);
	cafe_writel(cafe, CAFE_NAND_IRQ_CMD_DONE | CAFE_NAND_IRQ_DMA_DONE,
		    NAND_IRQ);
	if (cafe->usedma &&
	    (ctl1 & (CAFE_NAND_CTRL1_HAS_DATA_IN |
		     CAFE_NAND_CTRL1_HAS_DATA_OUT))) {
		uint32_t dmactl = CAFE_NAND_DMA_CTRL_ENABLE |
				  CAFE_NAND_DMA_CTRL_RESERVED;

		dmactl |= CAFE_FIELD_PREP(NAND_DMA_CTRL, DATA_LEN,
					  cafe->datalen);
		/* If WR or RD bits set, set up DMA */
		if (ctl1 & CAFE_NAND_CTRL1_HAS_DATA_IN) {
			/* It's a read */
			dmactl |= CAFE_NAND_DMA_CTRL_DATA_IN;
			/* ... so it's done when the DMA is done, not just
			   the command. */
			doneint = CAFE_NAND_IRQ_DMA_DONE;
		}
		cafe_writel(cafe, dmactl, NAND_DMA_CTRL);
	}
	cafe->datalen = 0;

	if (unlikely(regdebug)) {
		int i;
		printk("About to write command %08x to register 0\n", ctl1);
		for (i=4; i< 0x5c; i+=4)
			printk("Register %x: %08x\n", i, readl(cafe->mmio + i));
	}

	cafe_writel(cafe, ctl1, NAND_CTRL1);
	/* Apply this short delay always to ensure that we do wait tWB in
	 * any case on any machine. */
	ndelay(100);

	if (1) {
		int c;
		uint32_t irqs;

		for (c = 500000; c != 0; c--) {
			irqs = cafe_readl(cafe, NAND_IRQ);
			if (irqs & doneint)
				break;
			udelay(1);
			if (!(c % 100000))
				dev_dbg(&cafe->pdev->dev, "Wait for ready, IRQ %x\n", irqs);
			cpu_relax();
		}
		cafe_writel(cafe, doneint, NAND_IRQ);
		dev_dbg(&cafe->pdev->dev, "Command %x completed after %d usec, irqs %x (%x)\n",
			command, 500000-c, irqs, cafe_readl(cafe, NAND_IRQ));
	}

	WARN_ON(cafe->ctl2 & CAFE_NAND_CTRL2_AUTO_WRITE_ECC);

	switch (command) {

	case NAND_CMD_CACHEDPROG:
	case NAND_CMD_PAGEPROG:
	case NAND_CMD_ERASE1:
	case NAND_CMD_ERASE2:
	case NAND_CMD_SEQIN:
	case NAND_CMD_RNDIN:
	case NAND_CMD_STATUS:
	case NAND_CMD_RNDOUT:
		cafe_writel(cafe, cafe->ctl2, NAND_CTRL2);
		return;
	}
	nand_wait_ready(chip);
	cafe_writel(cafe, cafe->ctl2, NAND_CTRL2);
}

static void cafe_select_chip(struct nand_chip *chip, int chipnr)
{
	struct cafe_priv *cafe = nand_get_controller_data(chip);

	dev_dbg(&cafe->pdev->dev, "select_chip %d\n", chipnr);

	/* Mask the appropriate bit into the stored value of ctl1
	   which will be used by cafe_nand_cmdfunc() */
	cafe->ctl1 &= ~CAFE_NAND_CTRL1_CE;
	cafe->ctl1 |= CAFE_FIELD_PREP(NAND_CTRL1, CE, chipnr);
}

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
static int cafe_nand_read_page(struct nand_chip *chip, uint8_t *buf,
			       int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	unsigned int max_bitflips = 0;
	u32 ecc_result;

	dev_dbg(&cafe->pdev->dev, "ECC result %08x SYN1,2 %08x\n",
		cafe_readl(cafe, NAND_ECC_RESULT),
		cafe_readl(cafe, NAND_ECC_SYN_REG(0)));

	nand_read_page_op(chip, page, 0, buf, mtd->writesize);
	chip->legacy.read_buf(chip, chip->oob_poi, mtd->oobsize);

	ecc_result = cafe_readl(cafe, NAND_ECC_RESULT);
	if (checkecc && (ecc_result & CAFE_NAND_ECC_RESULT_RS_ERRORS)) {
		unsigned short syn[8], pat[4];
		int pos[4];
		u8 *oob = chip->oob_poi;
		int i, n;

		for (i=0; i<8; i+=2) {
			uint32_t tmp = cafe_readl(cafe, NAND_ECC_SYN_REG(i));
			uint16_t idx;

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
			dev_dbg(&cafe->pdev->dev, "Failed to correct ECC at %08x\n",
				cafe_readl(cafe, NAND_ADDR2) * 2048);
			for (i = 0; i < 0x5c; i += 4)
				printk("Register %x: %08x\n", i, readl(cafe->mmio + i));
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
static uint8_t cafe_bbt_pattern_2048[] = { 'B', 'b', 't', '0' };
static uint8_t cafe_mirror_pattern_2048[] = { '1', 't', 'b', 'B' };

static uint8_t cafe_bbt_pattern_512[] = { 0xBB };
static uint8_t cafe_mirror_pattern_512[] = { 0xBC };


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
				const uint8_t *buf, int oob_required,
				int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct cafe_priv *cafe = nand_get_controller_data(chip);
	int ret;

	nand_prog_page_begin_op(chip, page, 0, buf, mtd->writesize);
	chip->legacy.write_buf(chip, chip->oob_poi, mtd->oobsize);

	/* Set up ECC autogeneration */
	cafe->ctl2 |= CAFE_NAND_CTRL2_AUTO_WRITE_ECC;

	ret = nand_prog_page_end_op(chip);

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
	cafe_writel(cafe, lower_32_bits(cafe->dmaaddr), NAND_DMA_ADDR0);
	cafe_writel(cafe, upper_32_bits(cafe->dmaaddr), NAND_DMA_ADDR1);

	dev_dbg(&cafe->pdev->dev, "Set DMA address to %x (virt %p)\n",
		cafe_readl(cafe, NAND_DMA_ADDR0), cafe->dmabuf);

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

static const struct nand_controller_ops cafe_nand_controller_ops = {
	.attach_chip = cafe_nand_attach_chip,
	.detach_chip = cafe_nand_detach_chip,
};

static void cafe_nand_init(struct cafe_priv *cafe)
{
	u32 ctrl;

	/* Start off by resetting the NAND controller completely */
	cafe_writel(cafe, CAFE_GLOBAL_RESET_NAND, GLOBAL_RESET);
	cafe_writel(cafe, 0, GLOBAL_RESET);
	cafe_writel(cafe, 0xffffffff, NAND_IRQ_MASK);

	/* Restore timing configuration */
	cafe_writel(cafe, timing[0], NAND_TIMING1);
	cafe_writel(cafe, timing[1], NAND_TIMING2);
	cafe_writel(cafe, timing[2], NAND_TIMING3);

	/* Disable master reset, enable NAND clock */
	ctrl = cafe_readl(cafe, GLOBAL_CTRL);
	ctrl &= ~(CAFE_GLOBAL_SW_RESET_SET |
		  CAFE_GLOBAL_SW_RESET_CLEAR |
		  CAFE_GLOBAL_MASTER_RESET_SET |
		  CAFE_GLOBAL_MASTER_RESET_CLEAR |
		  CAFE_GLOBAL_NAND_CLK_ENABLE);
	ctrl |= CAFE_GLOBAL_NAND_CLK_ENABLE |
		CAFE_GLOBAL_SDH_CLK_ENABLE |
		CAFE_GLOBAL_CCIC_CLK_ENABLE;
	cafe_writel(cafe,
		    ctrl |
		    CAFE_GLOBAL_MASTER_RESET_SET |
		    CAFE_GLOBAL_SW_RESET_SET,
		    GLOBAL_CTRL);
	cafe_writel(cafe,
		    ctrl |
		    CAFE_GLOBAL_MASTER_RESET_CLEAR |
		    CAFE_GLOBAL_SW_RESET_CLEAR,
		    GLOBAL_CTRL);

	cafe_writel(cafe, 0, NAND_DMA_CTRL);

	cafe_writel(cafe,
		    CAFE_GLOBAL_NAND_CLK_ENABLE |
		    CAFE_GLOBAL_SDH_CLK_ENABLE |
		    CAFE_GLOBAL_CCIC_CLK_ENABLE |
		    CAFE_GLOBAL_MASTER_RESET_SET |
		    CAFE_GLOBAL_SW_RESET_CLEAR,
		    GLOBAL_CTRL);
	cafe_writel(cafe,
		    CAFE_GLOBAL_NAND_CLK_ENABLE |
		    CAFE_GLOBAL_SDH_CLK_ENABLE |
		    CAFE_GLOBAL_CCIC_CLK_ENABLE |
		    CAFE_GLOBAL_MASTER_RESET_CLEAR |
		    CAFE_GLOBAL_SW_RESET_CLEAR,
		    GLOBAL_CTRL);

	/* Set up DMA address */
	cafe_writel(cafe, lower_32_bits(cafe->dmaaddr), NAND_DMA_ADDR0);
	cafe_writel(cafe, upper_32_bits(cafe->dmaaddr), NAND_DMA_ADDR1);
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

	cafe->nand.legacy.cmdfunc = cafe_nand_cmdfunc;
	cafe->nand.legacy.dev_ready = cafe_device_ready;
	cafe->nand.legacy.read_byte = cafe_read_byte;
	cafe->nand.legacy.read_buf = cafe_read_buf;
	cafe->nand.legacy.write_buf = cafe_write_buf;
	cafe->nand.legacy.select_chip = cafe_select_chip;
	cafe->nand.legacy.set_features = nand_get_set_features_notsupp;
	cafe->nand.legacy.get_features = nand_get_set_features_notsupp;

	cafe->nand.legacy.chip_delay = 0;

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
		timing[0] = cafe_readl(cafe, NAND_TIMING1);
		timing[1] = cafe_readl(cafe, NAND_TIMING2);
		timing[2] = cafe_readl(cafe, NAND_TIMING3);

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
