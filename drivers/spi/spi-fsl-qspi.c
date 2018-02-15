/*
 * Freescale QuadSPI driver.
 *
 * Copyright (C) 2013 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

/* Controller needs driver to swap endian */
#define QUADSPI_QUIRK_SWAP_ENDIAN	(1 << 0)
/* Controller needs 4x internal clock */
#define QUADSPI_QUIRK_4X_INT_CLK	(1 << 1)
/*
 * TKT253890, Controller needs driver to fill txfifo till 16 byte to
 * trigger data transfer even though extern data will not transferred.
 */
#define QUADSPI_QUIRK_TKT253890		(1 << 2)
/* Controller cannot wake up from wait mode, TKT245618 */
#define QUADSPI_QUIRK_TKT245618         (1 << 3)

/* The registers */
#define QUADSPI_MCR			0x00
#define QUADSPI_MCR_RESERVED_SHIFT	16
#define QUADSPI_MCR_RESERVED_MASK	(0xF << QUADSPI_MCR_RESERVED_SHIFT)
#define QUADSPI_MCR_MDIS_SHIFT		14
#define QUADSPI_MCR_MDIS_MASK		(1 << QUADSPI_MCR_MDIS_SHIFT)
#define QUADSPI_MCR_CLR_TXF_SHIFT	11
#define QUADSPI_MCR_CLR_TXF_MASK	(1 << QUADSPI_MCR_CLR_TXF_SHIFT)
#define QUADSPI_MCR_CLR_RXF_SHIFT	10
#define QUADSPI_MCR_CLR_RXF_MASK	(1 << QUADSPI_MCR_CLR_RXF_SHIFT)
#define QUADSPI_MCR_DDR_EN_SHIFT	7
#define QUADSPI_MCR_DDR_EN_MASK		(1 << QUADSPI_MCR_DDR_EN_SHIFT)
#define QUADSPI_MCR_END_CFG_SHIFT	2
#define QUADSPI_MCR_END_CFG_MASK	(3 << QUADSPI_MCR_END_CFG_SHIFT)
#define QUADSPI_MCR_SWRSTHD_SHIFT	1
#define QUADSPI_MCR_SWRSTHD_MASK	(1 << QUADSPI_MCR_SWRSTHD_SHIFT)
#define QUADSPI_MCR_SWRSTSD_SHIFT	0
#define QUADSPI_MCR_SWRSTSD_MASK	(1 << QUADSPI_MCR_SWRSTSD_SHIFT)

#define QUADSPI_IPCR			0x08
#define QUADSPI_IPCR_SEQID_SHIFT	24
#define QUADSPI_IPCR_SEQID_MASK		(0xF << QUADSPI_IPCR_SEQID_SHIFT)

#define QUADSPI_BUF0CR			0x10
#define QUADSPI_BUF1CR			0x14
#define QUADSPI_BUF2CR			0x18
#define QUADSPI_BUFXCR_INVALID_MSTRID	0xe

#define QUADSPI_BUF3CR			0x1c
#define QUADSPI_BUF3CR_ALLMST_SHIFT	31
#define QUADSPI_BUF3CR_ALLMST_MASK	(1 << QUADSPI_BUF3CR_ALLMST_SHIFT)
#define QUADSPI_BUF3CR_ADATSZ_SHIFT		8
#define QUADSPI_BUF3CR_ADATSZ_MASK	(0xFF << QUADSPI_BUF3CR_ADATSZ_SHIFT)

#define QUADSPI_BFGENCR			0x20
#define QUADSPI_BFGENCR_PAR_EN_SHIFT	16
#define QUADSPI_BFGENCR_PAR_EN_MASK	(1 << (QUADSPI_BFGENCR_PAR_EN_SHIFT))
#define QUADSPI_BFGENCR_SEQID_SHIFT	12
#define QUADSPI_BFGENCR_SEQID_MASK	(0xF << QUADSPI_BFGENCR_SEQID_SHIFT)

#define QUADSPI_BUF0IND			0x30
#define QUADSPI_BUF1IND			0x34
#define QUADSPI_BUF2IND			0x38
#define QUADSPI_SFAR			0x100

#define QUADSPI_SMPR			0x108
#define QUADSPI_SMPR_DDRSMP_SHIFT	16
#define QUADSPI_SMPR_DDRSMP_MASK	(7 << QUADSPI_SMPR_DDRSMP_SHIFT)
#define QUADSPI_SMPR_FSDLY_SHIFT	6
#define QUADSPI_SMPR_FSDLY_MASK		(1 << QUADSPI_SMPR_FSDLY_SHIFT)
#define QUADSPI_SMPR_FSPHS_SHIFT	5
#define QUADSPI_SMPR_FSPHS_MASK		(1 << QUADSPI_SMPR_FSPHS_SHIFT)
#define QUADSPI_SMPR_HSENA_SHIFT	0
#define QUADSPI_SMPR_HSENA_MASK		(1 << QUADSPI_SMPR_HSENA_SHIFT)

#define QUADSPI_RBSR			0x10c
#define QUADSPI_RBSR_RDBFL_SHIFT	8
#define QUADSPI_RBSR_RDBFL_MASK		(0x3F << QUADSPI_RBSR_RDBFL_SHIFT)

#define QUADSPI_RBCT			0x110
#define QUADSPI_RBCT_WMRK_MASK		0x1F
#define QUADSPI_RBCT_RXBRD_SHIFT	8
#define QUADSPI_RBCT_RXBRD_USEIPS	(0x1 << QUADSPI_RBCT_RXBRD_SHIFT)

#define QUADSPI_TBSR			0x150
#define QUADSPI_TBDR			0x154
#define QUADSPI_SR			0x15c
#define QUADSPI_SR_IP_ACC_SHIFT		1
#define QUADSPI_SR_IP_ACC_MASK		(0x1 << QUADSPI_SR_IP_ACC_SHIFT)
#define QUADSPI_SR_AHB_ACC_SHIFT	2
#define QUADSPI_SR_AHB_ACC_MASK		(0x1 << QUADSPI_SR_AHB_ACC_SHIFT)

#define QUADSPI_FR			0x160
#define QUADSPI_FR_TFF_MASK		0x1

#define QUADSPI_SPTRCLR			0x16c

#define QUADSPI_SFA1AD			0x180
#define QUADSPI_SFA2AD			0x184
#define QUADSPI_SFB1AD			0x188
#define QUADSPI_SFB2AD			0x18c
#define QUADSPI_RBDR(x)			(0x200 + ((x) * 4))

#define QUADSPI_LUTKEY			0x300
#define QUADSPI_LUTKEY_VALUE		0x5AF05AF0

#define QUADSPI_LCKCR			0x304
#define QUADSPI_LCKER_LOCK		0x1
#define QUADSPI_LCKER_UNLOCK		0x2

#define QUADSPI_RSER			0x164
#define QUADSPI_RSER_TFIE		(0x1 << 0)

#define QUADSPI_LUT_BASE		0x310

/*
 * The definition of the LUT register shows below:
 *
 *  ---------------------------------------------------
 *  | INSTR1 | PAD1 | OPRND1 | INSTR0 | PAD0 | OPRND0 |
 *  ---------------------------------------------------
 */
#define OPRND0_SHIFT		0
#define PAD0_SHIFT		8
#define INSTR0_SHIFT		10
#define OPRND1_SHIFT		16

/* Instruction set for the LUT register. */
#define LUT_STOP		0
#define LUT_CMD			1
#define LUT_ADDR		2
#define LUT_DUMMY		3
#define LUT_MODE		4
#define LUT_MODE2		5
#define LUT_MODE4		6
#define LUT_FSL_READ		7
#define LUT_FSL_WRITE		8
#define LUT_JMP_ON_CS		9
#define LUT_ADDR_DDR		10
#define LUT_MODE_DDR		11
#define LUT_MODE2_DDR		12
#define LUT_MODE4_DDR		13
#define LUT_FSL_READ_DDR		14
#define LUT_FSL_WRITE_DDR		15
#define LUT_DATA_LEARN		16

/*
 * The PAD definitions for LUT register.
 *
 * The pad stands for the lines number of IO[0:3].
 * For example, the Quad read need four IO lines, so you should
 * set LUT_PAD4 which means we use four IO lines.
 */
#define LUT_PAD1		0
#define LUT_PAD2		1
#define LUT_PAD4		2
#define LUT_PAD(x)		(fls(x) - 1)

/* Oprands for the LUT register. */
#define ADDR24BIT		0x18
#define ADDR32BIT		0x20

/* Macros for constructing the LUT register. */
#define LUT_DEF(idx, ins, pad, opr) 					\
	((((ins) << 10) | ((pad) << 8) | (opr)) << (((idx) % 2) * 16))

/* other macros for LUT register. */
#define QUADSPI_LUT_REG(idx)	(QUADSPI_LUT_BASE + 0xf0 + (idx) * 4)
#define QUADSPI_LUT_NUM		64

/* SEQID -- we can have 16 seqids at most. */
#define SEQID_READ		0
#define SEQID_WREN		1
#define SEQID_WRDI		2
#define SEQID_RDSR		3
#define SEQID_SE		4
#define SEQID_CHIP_ERASE	5
#define SEQID_PP		6
#define SEQID_RDID		7
#define SEQID_WRSR		8
#define SEQID_RDCR		9
#define SEQID_EN4B		10
#define SEQID_BRWR		11

#define QUADSPI_MIN_IOMAP SZ_4M

enum fsl_qspi_devtype {
	FSL_QUADSPI_VYBRID,
	FSL_QUADSPI_IMX6SX,
	FSL_QUADSPI_IMX7D,
	FSL_QUADSPI_IMX6UL,
	FSL_QUADSPI_LS1021A,
};

struct fsl_qspi_devtype_data {
	enum fsl_qspi_devtype devtype;
	int rxfifo;
	int txfifo;
	int ahb_buf_size;
	int driver_data;
};

static const struct fsl_qspi_devtype_data vybrid_data = {
	.devtype = FSL_QUADSPI_VYBRID,
	.rxfifo = 128,
	.txfifo = 64,
	.ahb_buf_size = 1024,
	.driver_data = QUADSPI_QUIRK_SWAP_ENDIAN,
};

static const struct fsl_qspi_devtype_data imx6sx_data = {
	.devtype = FSL_QUADSPI_IMX6SX,
	.rxfifo = 128,
	.txfifo = 512,
	.ahb_buf_size = 1024,
	.driver_data = QUADSPI_QUIRK_4X_INT_CLK
		       | QUADSPI_QUIRK_TKT245618,
};

static const struct fsl_qspi_devtype_data imx7d_data = {
	.devtype = FSL_QUADSPI_IMX7D,
	.rxfifo = 512,
	.txfifo = 512,
	.ahb_buf_size = 1024,
	.driver_data = QUADSPI_QUIRK_TKT253890
		       | QUADSPI_QUIRK_4X_INT_CLK,
};

static const struct fsl_qspi_devtype_data imx6ul_data = {
	.devtype = FSL_QUADSPI_IMX6UL,
	.rxfifo = 128,
	.txfifo = 512,
	.ahb_buf_size = 1024,
	.driver_data = QUADSPI_QUIRK_TKT253890
		       | QUADSPI_QUIRK_4X_INT_CLK,
};

static struct fsl_qspi_devtype_data ls1021a_data = {
	.devtype = FSL_QUADSPI_LS1021A,
	.rxfifo = 128,
	.txfifo = 64,
	.ahb_buf_size = 1024,
	.driver_data = 0,
};

#define FSL_QSPI_MAX_CHIP	4
struct fsl_qspi {
	void __iomem *iobase;
	void __iomem *ahb_addr;
	u32 memmap_phy;
	u32 memmap_offs;
	u32 memmap_len;
	struct clk *clk, *clk_en;
	struct device *dev;
	struct completion c;
	const struct fsl_qspi_devtype_data *devtype_data;
	u32 clk_rate;
	unsigned int chip_base_addr; /* We may support two chips. */
	bool has_second_chip;
	bool big_endian;
	struct mutex lock;
	struct pm_qos_request pm_qos_req;
	int selected;
	u32 lut[4];
};

static inline int needs_swap_endian(struct fsl_qspi *q)
{
	return q->devtype_data->driver_data & QUADSPI_QUIRK_SWAP_ENDIAN;
}

static inline int needs_4x_clock(struct fsl_qspi *q)
{
	return q->devtype_data->driver_data & QUADSPI_QUIRK_4X_INT_CLK;
}

static inline int needs_fill_txfifo(struct fsl_qspi *q)
{
	return q->devtype_data->driver_data & QUADSPI_QUIRK_TKT253890;
}

static inline int needs_wakeup_wait_mode(struct fsl_qspi *q)
{
	return q->devtype_data->driver_data & QUADSPI_QUIRK_TKT245618;
}

/*
 * R/W functions for big- or little-endian registers:
 * The qSPI controller's endian is independent of the CPU core's endian.
 * So far, although the CPU core is little-endian but the qSPI have two
 * versions for big-endian and little-endian.
 */
static void qspi_writel(struct fsl_qspi *q, u32 val, void __iomem *addr)
{
	if (q->big_endian)
		iowrite32be(val, addr);
	else
		writel_relaxed(val, addr);
}

static u32 qspi_readl(struct fsl_qspi *q, void __iomem *addr)
{
	if (q->big_endian)
		return ioread32be(addr);
	else
		return readl_relaxed(addr);
}

/*
 * An IC bug makes us to re-arrange the 32-bit data.
 * The following chips, such as IMX6SLX, have fixed this bug.
 */
static inline u32 fsl_qspi_endian_xchg(struct fsl_qspi *q, u32 a)
{
	return needs_swap_endian(q) ? __swab32(a) : a;
}

static inline void fsl_qspi_unlock_lut(struct fsl_qspi *q)
{
	qspi_writel(q, QUADSPI_LUTKEY_VALUE, q->iobase + QUADSPI_LUTKEY);
	qspi_writel(q, QUADSPI_LCKER_UNLOCK, q->iobase + QUADSPI_LCKCR);
}

static inline void fsl_qspi_lock_lut(struct fsl_qspi *q)
{
	qspi_writel(q, QUADSPI_LUTKEY_VALUE, q->iobase + QUADSPI_LUTKEY);
	qspi_writel(q, QUADSPI_LCKER_LOCK, q->iobase + QUADSPI_LCKCR);
}

static irqreturn_t fsl_qspi_irq_handler(int irq, void *dev_id)
{
	struct fsl_qspi *q = dev_id;
	u32 reg;

	/* clear interrupt */
	reg = qspi_readl(q, q->iobase + QUADSPI_FR);
	qspi_writel(q, reg, q->iobase + QUADSPI_FR);

	if (reg & QUADSPI_FR_TFF_MASK)
		complete(&q->c);

	dev_dbg(q->dev, "QUADSPI_FR : 0x%.8x:0x%.8x\n", q->chip_base_addr, reg);
	return IRQ_HANDLED;
}

static int fsl_qspi_check_buswidth(struct fsl_qspi *q, u8 width)
{
	switch(width) {
	case 1:
	case 2:
	case 4:
		return 0;

	default:
		break;
	}

	return -ENOTSUPP;
}

static bool fsl_qspi_supports_op(struct spi_mem *mem,
				 const struct spi_mem_op *op)
{
	struct fsl_qspi *q = spi_controller_get_devdata(mem->spi->master);
	int ret;

	ret = fsl_qspi_check_buswidth(q, op->cmd.buswidth);

	if (op->addr.nbytes)
		ret |= fsl_qspi_check_buswidth(q, op->addr.buswidth);

	if (op->dummy.nbytes)
		ret |= fsl_qspi_check_buswidth(q, op->dummy.buswidth);

	if (op->data.nbytes)
		ret |= fsl_qspi_check_buswidth(q, op->data.buswidth);

	if (ret)
		return false;

	/* Max 24-bit address. */
	if (op->addr.nbytes > 3)
		return false;

	/* Max 64 dummy clock cycles. */
	if (op->dummy.nbytes * 8 / op->dummy.buswidth > 64)
		return false;

	if (op->data.dir == SPI_MEM_DATA_IN &&
	    (op->data.nbytes > q->devtype_data->ahb_buf_size ||
	     (op->data.nbytes > q->devtype_data->rxfifo - 4 &&
	      op->data.nbytes & 0x7)))
		return false;


	if (op->data.dir == SPI_MEM_DATA_OUT &&
	    op->data.nbytes > q->devtype_data->txfifo)
		return false;

	return true;
}

static void fsl_qspi_prepare_lut(struct fsl_qspi *q,
				 const struct spi_mem_op *op)
{
	void __iomem *base = q->iobase;
	u32 lutval[4] = { };
	int lutidx = 0, i;

	lutval[lutidx / 2] |= LUT_DEF(lutidx, LUT_CMD,
				      LUT_PAD(op->cmd.buswidth),
				      op->cmd.opcode);
	lutidx++;

	for (i = 0; i < op->addr.nbytes; i++) {
		u8 addrbyte = op->addr.val >> (8 * (op->addr.nbytes - i - 1));

		lutval[lutidx / 2] |= LUT_DEF(lutidx, LUT_MODE,
					      LUT_PAD(op->addr.buswidth),
					      addrbyte);
		lutidx++;
	}

/*
	if (op->addr.nbytes) {
		lutval[lutidx / 2] |= LUT_DEF(lutidx, LUT_ADDR,
					      LUT_PAD(op->addr.buswidth),
					      op->addr.nbytes * 8);
		lutidx++;
	}
*/

	if (op->dummy.nbytes) {
		lutval[lutidx / 2] |= LUT_DEF(lutidx, LUT_DUMMY,
					      LUT_PAD(op->dummy.buswidth),
					      op->dummy.nbytes * 8 /
					      op->dummy.buswidth);
		lutidx++;
	}

	if (op->data.nbytes) {
		lutval[lutidx / 2] |= LUT_DEF(lutidx,
					      op->data.dir == SPI_MEM_DATA_IN ?
					      LUT_FSL_READ : LUT_FSL_WRITE,
					      LUT_PAD(op->data.buswidth),
					      0);
		lutidx++;
	}

	lutval[lutidx / 2] |= LUT_DEF(lutidx, LUT_STOP, 0, 0);
	lutidx++;

	fsl_qspi_unlock_lut(q);
	for (i = 0; i < ARRAY_SIZE(lutval); i++)
		qspi_writel(q, lutval[i], base + QUADSPI_LUT_REG(i));
	fsl_qspi_lock_lut(q);
}

/* This function was used to prepare and enable QSPI clock */
static int fsl_qspi_clk_prep_enable(struct fsl_qspi *q)
{
	int ret;

	ret = clk_prepare_enable(q->clk_en);
	if (ret)
		return ret;

	ret = clk_prepare_enable(q->clk);
	if (ret) {
		clk_disable_unprepare(q->clk_en);
		return ret;
	}

	if (needs_wakeup_wait_mode(q))
		pm_qos_add_request(&q->pm_qos_req, PM_QOS_CPU_DMA_LATENCY, 0);

	return 0;
}

/* This function was used to disable and unprepare QSPI clock */
static void fsl_qspi_clk_disable_unprep(struct fsl_qspi *q)
{
	if (needs_wakeup_wait_mode(q))
		pm_qos_remove_request(&q->pm_qos_req);

	clk_disable_unprepare(q->clk);
	clk_disable_unprepare(q->clk_en);
}

static void fsl_qspi_select_mem(struct fsl_qspi *q, struct spi_device *spi)
{
	void __iomem *base = q->iobase;
	unsigned long rate = spi->max_speed_hz;
	int ret, i;

	if (q->selected == spi->chip_select)
		return;

	for (i = 0; i < 4; i++) {
		if (i < spi->chip_select)
			qspi_writel(q, q->memmap_phy,
				    base + QUADSPI_SFA1AD + (i * 4));
		else
			qspi_writel(q,
				    q->memmap_phy + 0x10000000,
				    base + QUADSPI_SFA1AD + (i * 4));
	}

	if (needs_4x_clock(q))
		rate *= 4;

	/* disable and unprepare clock to avoid glitch pass to controller */
	fsl_qspi_clk_disable_unprep(q);

	ret = clk_set_rate(q->clk, rate);
	if (ret)
		return;

	ret = fsl_qspi_clk_prep_enable(q);
	if (ret)
		return;

	q->selected = spi->chip_select;
}

static int fsl_qspi_exec_op(struct spi_mem *mem,
			    const struct spi_mem_op *op)
{
	struct fsl_qspi *q = spi_controller_get_devdata(mem->spi->master);
	void __iomem *base = q->iobase;
	int err, i, j;
	u32 val;

	mutex_lock(&q->lock);
//	fsl_qspi_clk_prep_enable(q);
	do {
		u32 status;

		status = qspi_readl(q, base + QUADSPI_SR);
		if (status &
		    (QUADSPI_SR_IP_ACC_MASK | QUADSPI_SR_AHB_ACC_MASK)) {
			udelay(1);
			dev_dbg(q->dev, "The controller is busy, 0x%x\n",
				status);
			continue;
		}
		break;
	} while (1);

	fsl_qspi_select_mem(q, mem->spi);

	qspi_writel(q, q->memmap_phy, base + QUADSPI_SFAR);
	qspi_writel(q,
		    qspi_readl(q, base + QUADSPI_MCR) |
		    QUADSPI_MCR_CLR_RXF_MASK | QUADSPI_MCR_CLR_TXF_MASK,
		    base + QUADSPI_MCR);

	if (op->data.nbytes > q->devtype_data->rxfifo - 4 &&
	    op->data.dir == SPI_MEM_DATA_IN) {
		unsigned int nbytes = ALIGN(op->data.nbytes, 8);
		static int seq = 0;

		if (nbytes > q->devtype_data->ahb_buf_size)
			return -ENOTSUPP;

		fsl_qspi_prepare_lut(q, op);
		qspi_writel(q, 0x11, base + QUADSPI_SPTRCLR);
		qspi_writel(q, 15 << 12, q->iobase + QUADSPI_BFGENCR);
		qspi_writel(q, QUADSPI_RBCT_WMRK_MASK, base + QUADSPI_RBCT);
		qspi_writel(q, QUADSPI_BUF3CR_ALLMST_MASK |
			    ((op->data.nbytes / 8) << QUADSPI_BUF3CR_ADATSZ_SHIFT),
			    base + QUADSPI_BUF3CR);
		memcpy_fromio(op->data.buf.in, q->ahb_addr + (seq * 0x1000), op->data.nbytes);
		seq = seq ? 0 : 1;
		err = 0;
		goto out;
	}

	qspi_writel(q, QUADSPI_RBCT_WMRK_MASK | QUADSPI_RBCT_RXBRD_USEIPS,
		    base + QUADSPI_RBCT);
	qspi_writel(q, 0x11, base + QUADSPI_SPTRCLR);
	fsl_qspi_prepare_lut(q, op);

	if (op->data.nbytes && op->data.dir == SPI_MEM_DATA_OUT) {
		const u8 *buf = op->data.buf.out;

		for (i = 0; i < op->data.nbytes; i += 4) {
			val = 0;
			for (j = 0; j < 4 && j + i < op->data.nbytes; j++)
				val |= buf[i + j] << (j * 8);

			qspi_writel(q, val, base + QUADSPI_TBDR);
		}
		for (; i < ALIGN(op->data.nbytes, 64); i += 4)
			qspi_writel(q, 0, base + QUADSPI_TBDR);
	}

	init_completion(&q->c);

	/*
	 * Always start the sequence at index 0 since we update the LUT at
	 * each exec_op() call. And no need to specify a DATA len, since it's
	 * already been specified in the LUT.
	 */
	qspi_writel(q, op->data.nbytes | (15 << 24), base + QUADSPI_IPCR);

	/* Wait for the interrupt. */
	if (!wait_for_completion_timeout(&q->c, msecs_to_jiffies(1000)))
		err = -ETIMEDOUT;
	else
		err = 0;

	if (!err && op->data.nbytes && op->data.dir == SPI_MEM_DATA_IN) {
		u8 *buf = op->data.buf.in;

		for (i = 0; i < op->data.nbytes; i += 4) {
			val = qspi_readl(q, base + QUADSPI_RBDR(i / 4));

			for (j = 0; j < 4 && j + i < op->data.nbytes; j++) {
				buf[i + j] = val >> (j * 8);
			}
		}
	}

out:
//	fsl_qspi_clk_disable_unprep(q);
	mutex_unlock(&q->lock);

	return err;
}

/* We use this function to do some basic init for spi_nor_scan(). */
static int fsl_qspi_default_setup(struct fsl_qspi *q)
{
	void __iomem *base = q->iobase;
	u32 reg;
	int ret;

	/* disable and unprepare clock to avoid glitch pass to controller */
	fsl_qspi_clk_disable_unprep(q);

	/* the default frequency, we will change it in the future. */
	ret = clk_set_rate(q->clk, 66000000);
	if (ret)
		return ret;

	ret = fsl_qspi_clk_prep_enable(q);
	if (ret)
		return ret;

	/* Reset the module */
	qspi_writel(q, QUADSPI_MCR_SWRSTSD_MASK | QUADSPI_MCR_SWRSTHD_MASK,
		base + QUADSPI_MCR);
	udelay(1);

	/* Disable the module */
	qspi_writel(q, QUADSPI_MCR_MDIS_MASK | QUADSPI_MCR_RESERVED_MASK,
			base + QUADSPI_MCR);

	reg = qspi_readl(q, base + QUADSPI_SMPR);
	qspi_writel(q, reg & ~(QUADSPI_SMPR_FSDLY_MASK
			| QUADSPI_SMPR_FSPHS_MASK
			| QUADSPI_SMPR_HSENA_MASK
			| QUADSPI_SMPR_DDRSMP_MASK), base + QUADSPI_SMPR);

	/* Enable the module */
	qspi_writel(q, QUADSPI_MCR_RESERVED_MASK | QUADSPI_MCR_END_CFG_MASK,
			base + QUADSPI_MCR);

	/* clear all interrupt status */
	qspi_writel(q, 0xffffffff, q->iobase + QUADSPI_FR);

	/* enable the interrupt */
	qspi_writel(q, QUADSPI_RSER_TFIE, q->iobase + QUADSPI_RSER);

	return 0;
}

static int fsl_qspi_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	struct fsl_qspi *q = spi_controller_get_devdata(mem->spi->master);

	if (op->data.dir == SPI_MEM_DATA_OUT) {
		if (op->data.nbytes > q->devtype_data->txfifo)
			op->data.nbytes = q->devtype_data->txfifo;
	} else {
		if (op->data.nbytes > q->devtype_data->ahb_buf_size) {
			op->data.nbytes = 1024;
		} else if (op->data.nbytes > q->devtype_data->rxfifo - 4) {
			op->data.nbytes &= ~0x7UL;
		}
	}

	return 0;
}

static const struct spi_controller_mem_ops fsl_qspi_mem_ops = {
	.adjust_op_size = fsl_qspi_adjust_op_size,
	.supports_op = fsl_qspi_supports_op,
	.exec_op = fsl_qspi_exec_op,
};

/*
 * There are two different ways to read out the data from the flash:
 *  the "IP Command Read" and the "AHB Command Read".
 *
 * The IC guy suggests we use the "AHB Command Read" which is faster
 * then the "IP Command Read". (What's more is that there is a bug in
 * the "IP Command Read" in the Vybrid.)
 *
 * After we set up the registers for the "AHB Command Read", we can use
 * the memcpy to read the data directly. A "missed" access to the buffer
 * causes the controller to clear the buffer, and use the sequence pointed
 * by the QUADSPI_BFGENCR[SEQID] to initiate a read from the flash.
 */
static void fsl_qspi_init_abh_read(struct fsl_qspi *q)
{
	void __iomem *base = q->iobase;

	/* AHB configuration for access buffer 0/1/2 .*/
	qspi_writel(q, QUADSPI_BUFXCR_INVALID_MSTRID, base + QUADSPI_BUF0CR);
	qspi_writel(q, QUADSPI_BUFXCR_INVALID_MSTRID, base + QUADSPI_BUF1CR);
	qspi_writel(q, QUADSPI_BUFXCR_INVALID_MSTRID, base + QUADSPI_BUF2CR);
	qspi_writel(q, QUADSPI_BUFXCR_INVALID_MSTRID, base + QUADSPI_BUF3CR);

	/* We only use the buffer3 */
	qspi_writel(q, 0, base + QUADSPI_BUF0IND);
	qspi_writel(q, 0, base + QUADSPI_BUF1IND);
	qspi_writel(q, 0, base + QUADSPI_BUF2IND);

	/* Set the default lut sequence for AHB Read. */
	qspi_writel(q, 0 << QUADSPI_BFGENCR_SEQID_SHIFT,
		    q->iobase + QUADSPI_BFGENCR);
}

static const struct of_device_id fsl_qspi_dt_ids[] = {
	{ .compatible = "fsl,vf610-qspi", .data = &vybrid_data, },
	{ .compatible = "fsl,imx6sx-qspi", .data = &imx6sx_data, },
	{ .compatible = "fsl,imx7d-qspi", .data = &imx7d_data, },
	{ .compatible = "fsl,imx6ul-qspi", .data = &imx6ul_data, },
	{ .compatible = "fsl,ls1021a-qspi", .data = (void *)&ls1021a_data, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fsl_qspi_dt_ids);

static int fsl_qspi_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct fsl_qspi *q;
	int ret;

	ctlr = spi_alloc_master(&pdev->dev, sizeof(*q));
	if (!ctlr)
		return -ENOMEM;

	ctlr->mode_bits = SPI_RX_DUAL | SPI_RX_QUAD |
			  SPI_TX_DUAL | SPI_TX_QUAD;
	q = spi_controller_get_devdata(ctlr);
	q->dev = dev;
	q->devtype_data = of_device_get_match_data(dev);
	if (!q->devtype_data) {
		ret = -ENODEV;
		goto err_put_ctrl;
	}

	platform_set_drvdata(pdev, q);

	/* find the resources */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "QuadSPI");
	q->iobase = devm_ioremap_resource(dev, res);
	if (IS_ERR(q->iobase)) {
		ret = PTR_ERR(q->iobase);
		goto err_put_ctrl;
	}

	q->big_endian = of_property_read_bool(np, "big-endian");
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					"QuadSPI-memory");
	q->ahb_addr = devm_ioremap_resource(dev, res);
	if (IS_ERR(q->ahb_addr)) {
		ret = PTR_ERR(q->iobase);
		goto err_put_ctrl;
	}
	/*
	if (!devm_request_mem_region(dev, res->start, resource_size(res),
				     res->name)) {
		dev_err(dev, "can't request region for resource %pR\n", res);
		ret = -EBUSY;
		goto err_put_ctrl;
	}
	*/

	q->memmap_phy = res->start;

	/* find the clocks */
	q->clk_en = devm_clk_get(dev, "qspi_en");
	if (IS_ERR(q->clk_en)) {
		ret = PTR_ERR(q->clk_en);
		goto err_put_ctrl;
	}

	q->clk = devm_clk_get(dev, "qspi");
	if (IS_ERR(q->clk)) {
		ret = PTR_ERR(q->clk);
		goto err_put_ctrl;
	}

	ret = fsl_qspi_clk_prep_enable(q);
	if (ret) {
		dev_err(dev, "can not enable the clock\n");
		goto err_put_ctrl;
	}

	/* find the irq */
	ret = platform_get_irq(pdev, 0);
	if (ret < 0) {
		dev_err(dev, "failed to get the irq: %d\n", ret);
		goto err_disable_clk;
	}

	ret = devm_request_irq(dev, ret,
			fsl_qspi_irq_handler, 0, pdev->name, q);
	if (ret) {
		dev_err(dev, "failed to request irq: %d\n", ret);
		goto err_disable_clk;
	}

	mutex_init(&q->lock);

	q->selected = -1;
	ctlr->bus_num = -1;
	ctlr->mem_ops = &fsl_qspi_mem_ops;

	if (of_get_property(np, "fsl,qspi-has-second-chip", NULL))
		ctlr->num_chipselect = 4;
	else
		ctlr->num_chipselect = 2;

	fsl_qspi_default_setup(q);
	fsl_qspi_init_abh_read(q);

	ctlr->dev.of_node = pdev->dev.of_node;
	ret = spi_register_controller(ctlr);
	if (ret)
		goto err_destroy_mutex;

//	fsl_qspi_clk_disable_unprep(q);

	return 0;

err_destroy_mutex:
	mutex_destroy(&q->lock);

err_disable_clk:
	fsl_qspi_clk_disable_unprep(q);

err_put_ctrl:
	spi_controller_put(ctlr);

	dev_err(dev, "Freescale QuadSPI probe failed\n");
	return ret;
}

static int fsl_qspi_remove(struct platform_device *pdev)
{
	struct fsl_qspi *q = platform_get_drvdata(pdev);

	/* disable the hardware */
	qspi_writel(q, QUADSPI_MCR_MDIS_MASK, q->iobase + QUADSPI_MCR);
	qspi_writel(q, 0x0, q->iobase + QUADSPI_RSER);

	mutex_destroy(&q->lock);

	if (q->ahb_addr)
		iounmap(q->ahb_addr);

	return 0;
}

static int fsl_qspi_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int fsl_qspi_resume(struct platform_device *pdev)
{
	int ret;
	struct fsl_qspi *q = platform_get_drvdata(pdev);

	q->selected = -1;
	ret = fsl_qspi_clk_prep_enable(q);
	if (ret)
		return ret;

	fsl_qspi_default_setup(q);

//	fsl_qspi_clk_disable_unprep(q);

	return 0;
}

static struct platform_driver fsl_qspi_driver = {
	.driver = {
		.name	= "fsl-quadspi",
		.of_match_table = fsl_qspi_dt_ids,
	},
	.probe          = fsl_qspi_probe,
	.remove		= fsl_qspi_remove,
	.suspend	= fsl_qspi_suspend,
	.resume		= fsl_qspi_resume,
};
module_platform_driver(fsl_qspi_driver);

MODULE_DESCRIPTION("Freescale QuadSPI Controller Driver");
MODULE_AUTHOR("Freescale Semiconductor Inc.");
MODULE_LICENSE("GPL v2");
