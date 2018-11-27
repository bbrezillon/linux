// SPDX-License-Identifier: GPL-2.0
/*
 * Based on m25p80.c, by Mike Lavender (mike@steroidmicros.com), with
 * influence from lart.c (Abraham Van Der Merwe) and mtd_dataflash.c
 *
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/math64.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include <linux/mtd/mtd.h>
#include <linux/of_platform.h>
#include <linux/spi/flash.h>
#include <linux/mtd/spi-nor.h>

#include "internals.h"

/* Define max times to check status register before we give up. */

/*
 * For everything but full-chip erase; probably could be much smaller, but kept
 * around for safety for now
 */
#define DEFAULT_READY_WAIT_JIFFIES		(40UL * HZ)

/*
 * For full-chip erase, calibrated to a 2MB flash (M25P16); should be scaled up
 * for larger flash
 */
#define CHIP_ERASE_2MB_READY_WAIT_JIFFIES	(40UL * HZ)

#define SPI_NOR_MAX_ADDR_WIDTH	4

#define JEDEC_MFR(info)	((info)->id[0])

/*
 * Read the status register, returning its value in the location
 * Return the status register value.
 * Returns negative if error occurred.
 */
static int read_sr(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_RDSR, &val, 1);
	if (ret < 0) {
		pr_err("error %d reading SR\n", (int) ret);
		return ret;
	}

	return val;
}

/*
 * Read the flag status register, returning its value in the location
 * Return the status register value.
 * Returns negative if error occurred.
 */
static int read_fsr(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_RDFSR, &val, 1);
	if (ret < 0) {
		pr_err("error %d reading FSR\n", ret);
		return ret;
	}

	return val;
}

/*
 * Read configuration register, returning its value in the
 * location. Return the configuration register value.
 * Returns negative if error occurred.
 */
static int read_cr(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_RDCR, &val, 1);
	if (ret < 0) {
		dev_err(nor->dev, "error %d reading CR\n", ret);
		return ret;
	}

	return val;
}

/*
 * Write status register 1 byte
 * Returns negative if error occurred.
 */
static int write_sr(struct spi_nor *nor, u8 val)
{
	nor->cmd_buf[0] = val;
	return nor->write_reg(nor, SPINOR_OP_WRSR, nor->cmd_buf, 1);
}

/*
 * Set write enable latch with Write Enable command.
 * Returns negative if error occurred.
 */
int write_enable(struct spi_nor *nor)
{
	return nor->write_reg(nor, SPINOR_OP_WREN, NULL, 0);
}

/*
 * Send write disable instruction to the chip.
 */
int write_disable(struct spi_nor *nor)
{
	return nor->write_reg(nor, SPINOR_OP_WRDI, NULL, 0);
}

static u8 spi_nor_convert_opcode(u8 opcode, const u8 table[][2], size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (table[i][0] == opcode)
			return table[i][1];

	/* No conversion found, keep input op code. */
	return opcode;
}

static u8 spi_nor_convert_3to4_read(u8 opcode)
{
	static const u8 spi_nor_3to4_read[][2] = {
		{ SPINOR_OP_READ,	SPINOR_OP_READ_4B },
		{ SPINOR_OP_READ_FAST,	SPINOR_OP_READ_FAST_4B },
		{ SPINOR_OP_READ_1_1_2,	SPINOR_OP_READ_1_1_2_4B },
		{ SPINOR_OP_READ_1_2_2,	SPINOR_OP_READ_1_2_2_4B },
		{ SPINOR_OP_READ_1_1_4,	SPINOR_OP_READ_1_1_4_4B },
		{ SPINOR_OP_READ_1_4_4,	SPINOR_OP_READ_1_4_4_4B },

		{ SPINOR_OP_READ_1_1_1_DTR,	SPINOR_OP_READ_1_1_1_DTR_4B },
		{ SPINOR_OP_READ_1_2_2_DTR,	SPINOR_OP_READ_1_2_2_DTR_4B },
		{ SPINOR_OP_READ_1_4_4_DTR,	SPINOR_OP_READ_1_4_4_DTR_4B },
	};

	return spi_nor_convert_opcode(opcode, spi_nor_3to4_read,
				      ARRAY_SIZE(spi_nor_3to4_read));
}

static u8 spi_nor_convert_3to4_program(u8 opcode)
{
	static const u8 spi_nor_3to4_program[][2] = {
		{ SPINOR_OP_PP,		SPINOR_OP_PP_4B },
		{ SPINOR_OP_PP_1_1_4,	SPINOR_OP_PP_1_1_4_4B },
		{ SPINOR_OP_PP_1_4_4,	SPINOR_OP_PP_1_4_4_4B },
	};

	return spi_nor_convert_opcode(opcode, spi_nor_3to4_program,
				      ARRAY_SIZE(spi_nor_3to4_program));
}

static u8 spi_nor_convert_3to4_erase(u8 opcode)
{
	static const u8 spi_nor_3to4_erase[][2] = {
		{ SPINOR_OP_BE_4K,	SPINOR_OP_BE_4K_4B },
		{ SPINOR_OP_BE_32K,	SPINOR_OP_BE_32K_4B },
		{ SPINOR_OP_SE,		SPINOR_OP_SE_4B },
	};

	return spi_nor_convert_opcode(opcode, spi_nor_3to4_erase,
				      ARRAY_SIZE(spi_nor_3to4_erase));
}

static void spi_nor_set_4byte_opcodes(struct spi_nor *nor)
{
	nor->read_opcode = spi_nor_convert_3to4_read(nor->read_opcode);
	nor->program_opcode = spi_nor_convert_3to4_program(nor->program_opcode);
	nor->erase_opcode = spi_nor_convert_3to4_erase(nor->erase_opcode);

	if (!spi_nor_has_uniform_erase(nor)) {
		struct spi_nor_erase_map *map = &nor->erase_map;
		struct spi_nor_erase_type *erase;
		int i;

		for (i = 0; i < SNOR_ERASE_TYPE_MAX; i++) {
			erase = &map->erase_type[i];
			erase->opcode =
				spi_nor_convert_3to4_erase(erase->opcode);
		}
	}
}

/* Enable/disable 4-byte addressing mode. */
static int set_4byte(struct spi_nor *nor, bool enable)
{
	if (nor->set_4byte)
		return nor->set_4byte(nor, enable);

	/*
	 * Spansion style. Should work for all NORs that do not have their own
	 * ->set_4byte() implementation.
	 */
	nor->cmd_buf[0] = enable << 7;
	return nor->write_reg(nor, SPINOR_OP_BRWR, nor->cmd_buf, 1);
}

static int spi_nor_xsr_ready(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_XRDSR, &val, 1);
	if (ret < 0) {
		dev_err(nor->dev, "error %d reading XRDSR\n", (int) ret);
		return ret;
	}

	return !!(val & XSR_RDY);
}

static int spi_nor_sr_ready(struct spi_nor *nor)
{
	int sr = read_sr(nor);
	if (sr < 0)
		return sr;

	if (nor->flags & SNOR_F_USE_CLSR && sr & (SR_E_ERR | SR_P_ERR)) {
		if (sr & SR_E_ERR)
			dev_err(nor->dev, "Erase Error occurred\n");
		else
			dev_err(nor->dev, "Programming Error occurred\n");

		nor->write_reg(nor, SPINOR_OP_CLSR, NULL, 0);
		return -EIO;
	}

	return !(sr & SR_WIP);
}

static int spi_nor_fsr_ready(struct spi_nor *nor)
{
	int fsr = read_fsr(nor);
	if (fsr < 0)
		return fsr;

	if (fsr & (FSR_E_ERR | FSR_P_ERR)) {
		if (fsr & FSR_E_ERR)
			dev_err(nor->dev, "Erase operation failed.\n");
		else
			dev_err(nor->dev, "Program operation failed.\n");

		if (fsr & FSR_PT_ERR)
			dev_err(nor->dev,
			"Attempted to modify a protected sector.\n");

		nor->write_reg(nor, SPINOR_OP_CLFSR, NULL, 0);
		return -EIO;
	}

	return fsr & FSR_READY;
}

static int spi_nor_ready(struct spi_nor *nor)
{
	int sr, fsr;

	if (nor->flags & SNOR_F_READY_XSR_RDY)
		sr = spi_nor_xsr_ready(nor);
	else
		sr = spi_nor_sr_ready(nor);
	if (sr < 0)
		return sr;
	fsr = nor->flags & SNOR_F_USE_FSR ? spi_nor_fsr_ready(nor) : 1;
	if (fsr < 0)
		return fsr;
	return sr && fsr;
}

/*
 * Service routine to read status register until ready, or timeout occurs.
 * Returns non-zero if error.
 */
static int spi_nor_wait_till_ready_with_timeout(struct spi_nor *nor,
						unsigned long timeout_jiffies)
{
	unsigned long deadline;
	int timeout = 0, ret;

	deadline = jiffies + timeout_jiffies;

	while (!timeout) {
		if (time_after_eq(jiffies, deadline))
			timeout = 1;

		ret = spi_nor_ready(nor);
		if (ret < 0)
			return ret;
		if (ret)
			return 0;

		cond_resched();
	}

	dev_err(nor->dev, "flash operation timed out\n");

	return -ETIMEDOUT;
}

int spi_nor_wait_till_ready(struct spi_nor *nor)
{
	return spi_nor_wait_till_ready_with_timeout(nor,
						    DEFAULT_READY_WAIT_JIFFIES);
}

/*
 * Erase the whole flash memory
 *
 * Returns 0 if successful, non-zero otherwise.
 */
static int erase_chip(struct spi_nor *nor)
{
	dev_dbg(nor->dev, " %lldKiB\n", (long long)(nor->mtd.size >> 10));

	return nor->write_reg(nor, SPINOR_OP_CHIP_ERASE, NULL, 0);
}

int spi_nor_lock_and_prep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	int ret = 0;

	mutex_lock(&nor->lock);

	if (nor->prepare) {
		ret = nor->prepare(nor, ops);
		if (ret) {
			dev_err(nor->dev, "failed in the preparation.\n");
			mutex_unlock(&nor->lock);
			return ret;
		}
	}
	return ret;
}

void spi_nor_unlock_and_unprep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	if (nor->unprepare)
		nor->unprepare(nor, ops);
	mutex_unlock(&nor->lock);
}

/*
 * This code converts an address to the Default Address Mode, that has non
 * power of two page sizes. We must support this mode because it is the default
 * mode supported by Xilinx tools, it can access the whole flash area and
 * changing over to the Power-of-two mode is irreversible and corrupts the
 * original data.
 * Addr can safely be unsigned int, the biggest S3AN device is smaller than
 * 4 MiB.
 */
static u32 s3an_convert_addr(struct spi_nor *nor, u32 addr)
{
	u32 offset, page;

	offset = addr % nor->page_size;
	page = addr / nor->page_size;
	page <<= (nor->page_size > 512) ? 10 : 9;

	return page | offset;
}

static u32 spi_nor_convert_addr(struct spi_nor *nor, u32 addr)
{
	if (!nor->convert_addr)
		return addr;

	return nor->convert_addr(nor, addr);
}

/*
 * Initiate the erasure of a single sector
 */
static int spi_nor_erase_sector(struct spi_nor *nor, u32 addr)
{
	u8 buf[SPI_NOR_MAX_ADDR_WIDTH];
	int i;

	addr = spi_nor_convert_addr(nor, addr);

	if (nor->erase)
		return nor->erase(nor, addr);

	/*
	 * Default implementation, if driver doesn't have a specialized HW
	 * control
	 */
	for (i = nor->addr_width - 1; i >= 0; i--) {
		buf[i] = addr & 0xff;
		addr >>= 8;
	}

	return nor->write_reg(nor, nor->erase_opcode, buf, nor->addr_width);
}

/**
 * spi_nor_div_by_erase_size() - calculate remainder and update new dividend
 * @erase:	pointer to a structure that describes a SPI NOR erase type
 * @dividend:	dividend value
 * @remainder:	pointer to u32 remainder (will be updated)
 *
 * Return: the result of the division
 */
static u64 spi_nor_div_by_erase_size(const struct spi_nor_erase_type *erase,
				     u64 dividend, u32 *remainder)
{
	/* JEDEC JESD216B Standard imposes erase sizes to be power of 2. */
	*remainder = (u32)dividend & erase->size_mask;
	return dividend >> erase->size_shift;
}

/**
 * spi_nor_find_best_erase_type() - find the best erase type for the given
 *				    offset in the serial flash memory and the
 *				    number of bytes to erase. The region in
 *				    which the address fits is expected to be
 *				    provided.
 * @map:	the erase map of the SPI NOR
 * @region:	pointer to a structure that describes a SPI NOR erase region
 * @addr:	offset in the serial flash memory
 * @len:	number of bytes to erase
 *
 * Return: a pointer to the best fitted erase type, NULL otherwise.
 */
static const struct spi_nor_erase_type *
spi_nor_find_best_erase_type(const struct spi_nor_erase_map *map,
			     const struct spi_nor_erase_region *region,
			     u64 addr, u32 len)
{
	const struct spi_nor_erase_type *erase;
	u32 rem;
	int i;
	u8 erase_mask = region->offset & SNOR_ERASE_TYPE_MASK;

	/*
	 * Erase types are ordered by size, with the biggest erase type at
	 * index 0.
	 */
	for (i = SNOR_ERASE_TYPE_MAX - 1; i >= 0; i--) {
		/* Does the erase region support the tested erase type? */
		if (!(erase_mask & BIT(i)))
			continue;

		erase = &map->erase_type[i];

		/* Don't erase more than what the user has asked for. */
		if (erase->size > len)
			continue;

		/* Alignment is not mandatory for overlaid regions */
		if (region->offset & SNOR_OVERLAID_REGION)
			return erase;

		spi_nor_div_by_erase_size(erase, addr, &rem);
		if (rem)
			continue;
		else
			return erase;
	}

	return NULL;
}

/**
 * spi_nor_region_next() - get the next spi nor region
 * @region:	pointer to a structure that describes a SPI NOR erase region
 *
 * Return: the next spi nor region or NULL if last region.
 */
static struct spi_nor_erase_region *
spi_nor_region_next(struct spi_nor_erase_region *region)
{
	if (spi_nor_region_is_last(region))
		return NULL;
	region++;
	return region;
}

/**
 * spi_nor_find_erase_region() - find the region of the serial flash memory in
 *				 which the offset fits
 * @map:	the erase map of the SPI NOR
 * @addr:	offset in the serial flash memory
 *
 * Return: a pointer to the spi_nor_erase_region struct, ERR_PTR(-errno)
 *	   otherwise.
 */
static struct spi_nor_erase_region *
spi_nor_find_erase_region(const struct spi_nor_erase_map *map, u64 addr)
{
	struct spi_nor_erase_region *region = map->regions;
	u64 region_start = region->offset & ~SNOR_ERASE_FLAGS_MASK;
	u64 region_end = region_start + region->size;

	while (addr < region_start || addr >= region_end) {
		region = spi_nor_region_next(region);
		if (!region)
			return ERR_PTR(-EINVAL);

		region_start = region->offset & ~SNOR_ERASE_FLAGS_MASK;
		region_end = region_start + region->size;
	}

	return region;
}

/**
 * spi_nor_init_erase_cmd() - initialize an erase command
 * @region:	pointer to a structure that describes a SPI NOR erase region
 * @erase:	pointer to a structure that describes a SPI NOR erase type
 *
 * Return: the pointer to the allocated erase command, ERR_PTR(-errno)
 *	   otherwise.
 */
static struct spi_nor_erase_command *
spi_nor_init_erase_cmd(const struct spi_nor_erase_region *region,
		       const struct spi_nor_erase_type *erase)
{
	struct spi_nor_erase_command *cmd;

	cmd = kmalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&cmd->list);
	cmd->opcode = erase->opcode;
	cmd->count = 1;

	if (region->offset & SNOR_OVERLAID_REGION)
		cmd->size = region->size;
	else
		cmd->size = erase->size;

	return cmd;
}

/**
 * spi_nor_destroy_erase_cmd_list() - destroy erase command list
 * @erase_list:	list of erase commands
 */
static void spi_nor_destroy_erase_cmd_list(struct list_head *erase_list)
{
	struct spi_nor_erase_command *cmd, *next;

	list_for_each_entry_safe(cmd, next, erase_list, list) {
		list_del(&cmd->list);
		kfree(cmd);
	}
}

/**
 * spi_nor_init_erase_cmd_list() - initialize erase command list
 * @nor:	pointer to a 'struct spi_nor'
 * @erase_list:	list of erase commands to be executed once we validate that the
 *		erase can be performed
 * @addr:	offset in the serial flash memory
 * @len:	number of bytes to erase
 *
 * Builds the list of best fitted erase commands and verifies if the erase can
 * be performed.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_init_erase_cmd_list(struct spi_nor *nor,
				       struct list_head *erase_list,
				       u64 addr, u32 len)
{
	const struct spi_nor_erase_map *map = &nor->erase_map;
	const struct spi_nor_erase_type *erase, *prev_erase = NULL;
	struct spi_nor_erase_region *region;
	struct spi_nor_erase_command *cmd = NULL;
	u64 region_end;
	int ret = -EINVAL;

	region = spi_nor_find_erase_region(map, addr);
	if (IS_ERR(region))
		return PTR_ERR(region);

	region_end = spi_nor_region_end(region);

	while (len) {
		erase = spi_nor_find_best_erase_type(map, region, addr, len);
		if (!erase)
			goto destroy_erase_cmd_list;

		if (prev_erase != erase ||
		    region->offset & SNOR_OVERLAID_REGION) {
			cmd = spi_nor_init_erase_cmd(region, erase);
			if (IS_ERR(cmd)) {
				ret = PTR_ERR(cmd);
				goto destroy_erase_cmd_list;
			}

			list_add_tail(&cmd->list, erase_list);
		} else {
			cmd->count++;
		}

		addr += cmd->size;
		len -= cmd->size;

		if (len && addr >= region_end) {
			region = spi_nor_region_next(region);
			if (!region)
				goto destroy_erase_cmd_list;
			region_end = spi_nor_region_end(region);
		}

		prev_erase = erase;
	}

	return 0;

destroy_erase_cmd_list:
	spi_nor_destroy_erase_cmd_list(erase_list);
	return ret;
}

/**
 * spi_nor_erase_multi_sectors() - perform a non-uniform erase
 * @nor:	pointer to a 'struct spi_nor'
 * @addr:	offset in the serial flash memory
 * @len:	number of bytes to erase
 *
 * Build a list of best fitted erase commands and execute it once we validate
 * that the erase can be performed.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_erase_multi_sectors(struct spi_nor *nor, u64 addr, u32 len)
{
	LIST_HEAD(erase_list);
	struct spi_nor_erase_command *cmd, *next;
	int ret;

	ret = spi_nor_init_erase_cmd_list(nor, &erase_list, addr, len);
	if (ret)
		return ret;

	list_for_each_entry_safe(cmd, next, &erase_list, list) {
		nor->erase_opcode = cmd->opcode;
		while (cmd->count) {
			write_enable(nor);

			ret = spi_nor_erase_sector(nor, addr);
			if (ret)
				goto destroy_erase_cmd_list;

			addr += cmd->size;
			cmd->count--;

			ret = spi_nor_wait_till_ready(nor);
			if (ret)
				goto destroy_erase_cmd_list;
		}
		list_del(&cmd->list);
		kfree(cmd);
	}

	return 0;

destroy_erase_cmd_list:
	spi_nor_destroy_erase_cmd_list(&erase_list);
	return ret;
}

/*
 * Erase an address range on the nor chip.  The address range may extend
 * one or more erase sectors.  Return an error is there is a problem erasing.
 */
static int spi_nor_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	u32 addr, len;
	uint32_t rem;
	int ret;

	dev_dbg(nor->dev, "at 0x%llx, len %lld\n", (long long)instr->addr,
			(long long)instr->len);

	if (spi_nor_has_uniform_erase(nor)) {
		div_u64_rem(instr->len, mtd->erasesize, &rem);
		if (rem)
			return -EINVAL;
	}

	addr = instr->addr;
	len = instr->len;

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_ERASE);
	if (ret)
		return ret;

	/* whole-chip erase? */
	if (len == mtd->size && !(nor->flags & SNOR_F_NO_OP_CHIP_ERASE)) {
		unsigned long timeout;

		write_enable(nor);

		if (erase_chip(nor)) {
			ret = -EIO;
			goto erase_err;
		}

		/*
		 * Scale the timeout linearly with the size of the flash, with
		 * a minimum calibrated to an old 2MB flash. We could try to
		 * pull these from CFI/SFDP, but these values should be good
		 * enough for now.
		 */
		timeout = max(CHIP_ERASE_2MB_READY_WAIT_JIFFIES,
			      CHIP_ERASE_2MB_READY_WAIT_JIFFIES *
			      (unsigned long)(mtd->size / SZ_2M));
		ret = spi_nor_wait_till_ready_with_timeout(nor, timeout);
		if (ret)
			goto erase_err;

	/* REVISIT in some cases we could speed up erasing large regions
	 * by using SPINOR_OP_SE instead of SPINOR_OP_BE_4K.  We may have set up
	 * to use "small sector erase", but that's not always optimal.
	 */

	/* "sector"-at-a-time erase */
	} else if (spi_nor_has_uniform_erase(nor)) {
		while (len) {
			write_enable(nor);

			ret = spi_nor_erase_sector(nor, addr);
			if (ret)
				goto erase_err;

			addr += mtd->erasesize;
			len -= mtd->erasesize;

			ret = spi_nor_wait_till_ready(nor);
			if (ret)
				goto erase_err;
		}

	/* erase multiple sectors */
	} else {
		ret = spi_nor_erase_multi_sectors(nor, addr, len);
		if (ret)
			goto erase_err;
	}

	write_disable(nor);

erase_err:
	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_ERASE);

	return ret;
}

/* Write status register and ensure bits in mask match written values */
static int write_sr_and_check(struct spi_nor *nor, u8 status_new, u8 mask)
{
	int ret;

	write_enable(nor);
	ret = write_sr(nor, status_new);
	if (ret)
		return ret;

	ret = spi_nor_wait_till_ready(nor);
	if (ret)
		return ret;

	ret = read_sr(nor);
	if (ret < 0)
		return ret;

	return ((ret & mask) != (status_new & mask)) ? -EIO : 0;
}

static void get_locked_range_sr(struct spi_nor *nor, u8 sr, loff_t *ofs,
				uint64_t *len)
{
	struct mtd_info *mtd = &nor->mtd;
	u8 mask = SR_BP2 | SR_BP1 | SR_BP0;
	int shift = ffs(mask) - 1;
	int pow;

	if (!(sr & mask)) {
		/* No protection */
		*ofs = 0;
		*len = 0;
	} else {
		pow = ((sr & mask) ^ mask) >> shift;
		*len = mtd->size >> pow;
		if (nor->flags & SNOR_F_HAS_SR_TB && sr & SR_TB)
			*ofs = 0;
		else
			*ofs = mtd->size - *len;
	}
}

/*
 * Return 1 if the entire region is locked (if @locked is true) or unlocked (if
 * @locked is false); 0 otherwise
 */
static int check_lock_status_sr(struct spi_nor *nor, loff_t ofs, uint64_t len,
				u8 sr, bool locked)
{
	loff_t lock_offs;
	uint64_t lock_len;

	if (!len)
		return 1;

	get_locked_range_sr(nor, sr, &lock_offs, &lock_len);

	if (locked)
		/* Requested range is a sub-range of locked range */
		return (ofs + len <= lock_offs + lock_len) && (ofs >= lock_offs);
	else
		/* Requested range does not overlap with locked range */
		return (ofs >= lock_offs + lock_len) || (ofs + len <= lock_offs);
}

static int is_locked_sr(struct spi_nor *nor, loff_t ofs, uint64_t len, u8 sr)
{
	return check_lock_status_sr(nor, ofs, len, sr, true);
}

static int is_unlocked_sr(struct spi_nor *nor, loff_t ofs, uint64_t len, u8 sr)
{
	return check_lock_status_sr(nor, ofs, len, sr, false);
}

/*
 * Lock a region of the flash. Compatible with ST Micro and similar flash.
 * Supports the block protection bits BP{0,1,2} in the status register
 * (SR). Does not support these features found in newer SR bitfields:
 *   - SEC: sector/block protect - only handle SEC=0 (block protect)
 *   - CMP: complement protect - only support CMP=0 (range is not complemented)
 *
 * Support for the following is provided conditionally for some flash:
 *   - TB: top/bottom protect
 *
 * Sample table portion for 8MB flash (Winbond w25q64fw):
 *
 *   SEC  |  TB   |  BP2  |  BP1  |  BP0  |  Prot Length  | Protected Portion
 *  --------------------------------------------------------------------------
 *    X   |   X   |   0   |   0   |   0   |  NONE         | NONE
 *    0   |   0   |   0   |   0   |   1   |  128 KB       | Upper 1/64
 *    0   |   0   |   0   |   1   |   0   |  256 KB       | Upper 1/32
 *    0   |   0   |   0   |   1   |   1   |  512 KB       | Upper 1/16
 *    0   |   0   |   1   |   0   |   0   |  1 MB         | Upper 1/8
 *    0   |   0   |   1   |   0   |   1   |  2 MB         | Upper 1/4
 *    0   |   0   |   1   |   1   |   0   |  4 MB         | Upper 1/2
 *    X   |   X   |   1   |   1   |   1   |  8 MB         | ALL
 *  ------|-------|-------|-------|-------|---------------|-------------------
 *    0   |   1   |   0   |   0   |   1   |  128 KB       | Lower 1/64
 *    0   |   1   |   0   |   1   |   0   |  256 KB       | Lower 1/32
 *    0   |   1   |   0   |   1   |   1   |  512 KB       | Lower 1/16
 *    0   |   1   |   1   |   0   |   0   |  1 MB         | Lower 1/8
 *    0   |   1   |   1   |   0   |   1   |  2 MB         | Lower 1/4
 *    0   |   1   |   1   |   1   |   0   |  4 MB         | Lower 1/2
 *
 * Returns negative on errors, 0 on success.
 */
static int sr_lock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct mtd_info *mtd = &nor->mtd;
	int status_old, status_new;
	u8 mask = SR_BP2 | SR_BP1 | SR_BP0;
	u8 shift = ffs(mask) - 1, pow, val;
	loff_t lock_len;
	bool can_be_top = true, can_be_bottom = nor->flags & SNOR_F_HAS_SR_TB;
	bool use_top;

	status_old = read_sr(nor);
	if (status_old < 0)
		return status_old;

	/* If nothing in our range is unlocked, we don't need to do anything */
	if (is_locked_sr(nor, ofs, len, status_old))
		return 0;

	/* If anything below us is unlocked, we can't use 'bottom' protection */
	if (!is_locked_sr(nor, 0, ofs, status_old))
		can_be_bottom = false;

	/* If anything above us is unlocked, we can't use 'top' protection */
	if (!is_locked_sr(nor, ofs + len, mtd->size - (ofs + len),
			  status_old))
		can_be_top = false;

	if (!can_be_bottom && !can_be_top)
		return -EINVAL;

	/* Prefer top, if both are valid */
	use_top = can_be_top;

	/* lock_len: length of region that should end up locked */
	if (use_top)
		lock_len = mtd->size - ofs;
	else
		lock_len = ofs + len;

	/*
	 * Need smallest pow such that:
	 *
	 *   1 / (2^pow) <= (len / size)
	 *
	 * so (assuming power-of-2 size) we do:
	 *
	 *   pow = ceil(log2(size / len)) = log2(size) - floor(log2(len))
	 */
	pow = ilog2(mtd->size) - ilog2(lock_len);
	val = mask - (pow << shift);
	if (val & ~mask)
		return -EINVAL;
	/* Don't "lock" with no region! */
	if (!(val & mask))
		return -EINVAL;

	status_new = (status_old & ~mask & ~SR_TB) | val;

	/* Disallow further writes if WP pin is asserted */
	status_new |= SR_SRWD;

	if (!use_top)
		status_new |= SR_TB;

	/* Don't bother if they're the same */
	if (status_new == status_old)
		return 0;

	/* Only modify protection if it will not unlock other areas */
	if ((status_new & mask) < (status_old & mask))
		return -EINVAL;

	return write_sr_and_check(nor, status_new, mask);
}

/*
 * Unlock a region of the flash. See sr_lock() for more info
 *
 * Returns negative on errors, 0 on success.
 */
static int sr_unlock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct mtd_info *mtd = &nor->mtd;
	int status_old, status_new;
	u8 mask = SR_BP2 | SR_BP1 | SR_BP0;
	u8 shift = ffs(mask) - 1, pow, val;
	loff_t lock_len;
	bool can_be_top = true, can_be_bottom = nor->flags & SNOR_F_HAS_SR_TB;
	bool use_top;

	status_old = read_sr(nor);
	if (status_old < 0)
		return status_old;

	/* If nothing in our range is locked, we don't need to do anything */
	if (is_unlocked_sr(nor, ofs, len, status_old))
		return 0;

	/* If anything below us is locked, we can't use 'top' protection */
	if (!is_unlocked_sr(nor, 0, ofs, status_old))
		can_be_top = false;

	/* If anything above us is locked, we can't use 'bottom' protection */
	if (!is_unlocked_sr(nor, ofs + len, mtd->size - (ofs + len),
			    status_old))
		can_be_bottom = false;

	if (!can_be_bottom && !can_be_top)
		return -EINVAL;

	/* Prefer top, if both are valid */
	use_top = can_be_top;

	/* lock_len: length of region that should remain locked */
	if (use_top)
		lock_len = mtd->size - (ofs + len);
	else
		lock_len = ofs;

	/*
	 * Need largest pow such that:
	 *
	 *   1 / (2^pow) >= (len / size)
	 *
	 * so (assuming power-of-2 size) we do:
	 *
	 *   pow = floor(log2(size / len)) = log2(size) - ceil(log2(len))
	 */
	pow = ilog2(mtd->size) - order_base_2(lock_len);
	if (lock_len == 0) {
		val = 0; /* fully unlocked */
	} else {
		val = mask - (pow << shift);
		/* Some power-of-two sizes are not supported */
		if (val & ~mask)
			return -EINVAL;
	}

	status_new = (status_old & ~mask & ~SR_TB) | val;

	/* Don't protect status register if we're fully unlocked */
	if (lock_len == 0)
		status_new &= ~SR_SRWD;

	if (!use_top)
		status_new |= SR_TB;

	/* Don't bother if they're the same */
	if (status_new == status_old)
		return 0;

	/* Only modify protection if it will not lock other areas */
	if ((status_new & mask) > (status_old & mask))
		return -EINVAL;

	return write_sr_and_check(nor, status_new, mask);
}

/*
 * Check if a region of the flash is (completely) locked. See sr_lock() for
 * more info.
 *
 * Returns 1 if entire region is locked, 0 if any portion is unlocked, and
 * negative on errors.
 */
static int sr_is_locked(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	int status;

	status = read_sr(nor);
	if (status < 0)
		return status;

	return is_locked_sr(nor, ofs, len, status);
}

static const struct spi_nor_locking_ops sr_locking_ops = {
	.lock = sr_lock,
	.unlock = sr_unlock,
	.is_locked = sr_is_locked,
};

static int spi_nor_lock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	int ret;

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_LOCK);
	if (ret)
		return ret;

	ret = nor->locking_ops->lock(nor, ofs, len);

	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_UNLOCK);
	return ret;
}

static int spi_nor_unlock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	int ret;

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_UNLOCK);
	if (ret)
		return ret;

	ret = nor->locking_ops->unlock(nor, ofs, len);

	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_LOCK);
	return ret;
}

static int spi_nor_is_locked(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	int ret;

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_UNLOCK);
	if (ret)
		return ret;

	ret = nor->locking_ops->is_locked(nor, ofs, len);

	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_LOCK);
	return ret;
}

/*
 * Write status Register and configuration register with 2 bytes
 * The first byte will be written to the status register, while the
 * second byte will be written to the configuration register.
 * Return negative if error occurred.
 */
static int write_sr_cr(struct spi_nor *nor, u8 *sr_cr)
{
	int ret;

	write_enable(nor);

	ret = nor->write_reg(nor, SPINOR_OP_WRSR, sr_cr, 2);
	if (ret < 0) {
		dev_err(nor->dev,
			"error while writing configuration register\n");
		return -EINVAL;
	}

	ret = spi_nor_wait_till_ready(nor);
	if (ret) {
		dev_err(nor->dev,
			"timeout while writing configuration register\n");
		return ret;
	}

	return 0;
}

int no_quad_enable(struct spi_nor *nor)
{
	return 0;
}

/**
 * sr1_bit6_quad_enable() - set QE bit in Status Register.
 * @nor:	pointer to a 'struct spi_nor'
 *
 * Set the Quad Enable (QE) bit in the Status Register.
 *
 * bit 6 of the Status Register is the QE bit for Macronix like QSPI memories.
 *
 * Return: 0 on success, -errno otherwise.
 */
int sr1_bit6_quad_enable(struct spi_nor *nor)
{
	int ret, val;

	val = read_sr(nor);
	if (val < 0)
		return val;
	if (val & SR_QUAD_EN_MX)
		return 0;

	write_enable(nor);

	write_sr(nor, val | SR_QUAD_EN_MX);

	ret = spi_nor_wait_till_ready(nor);
	if (ret)
		return ret;

	ret = read_sr(nor);
	if (!(ret > 0 && (ret & SR_QUAD_EN_MX))) {
		dev_err(nor->dev, "QE bit not set\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * legacy_quad_enable() - set QE bit in Configuraiton Register.
 * @nor:	pointer to a 'struct spi_nor'
 *
 * Set the Quad Enable (QE) bit in the Configuration Register.
 * This function is kept for legacy purpose because it has been used for a
 * long time without anybody complaining but it should be considered as
 * deprecated and maybe buggy.
 * First, this function doesn't care about the previous values of the Status
 * and Configuration Registers when it sets the QE bit (bit 1) in the
 * Configuration Register: all other bits are cleared, which may have unwanted
 * side effects like removing some block protections.
 * Secondly, it uses the Read Configuration Register (35h) instruction though
 * some very old and few memories don't support this instruction. If a pull-up
 * resistor is present on the MISO/IO1 line, we might still be able to pass the
 * "read back" test because the QSPI memory doesn't recognize the command,
 * so leaves the MISO/IO1 line state unchanged, hence read_cr() returns 0xFF.
 *
 * bit 1 of the Configuration Register is the QE bit for Spansion like QSPI
 * memories.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int legacy_quad_enable(struct spi_nor *nor)
{
	u8 sr_cr[2] = {0, CR_QUAD_EN_SPAN};
	int ret;

	ret = write_sr_cr(nor, sr_cr);
	if (ret)
		return ret;

	/* read back and check it */
	ret = read_cr(nor);
	if (!(ret > 0 && (ret & CR_QUAD_EN_SPAN))) {
		dev_err(nor->dev, "Spansion Quad bit not set\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * sr2_bit1_no_read_quad_enable() - set QE bit in Configuration Register.
 * @nor:	pointer to a 'struct spi_nor'
 *
 * Set the Quad Enable (QE) bit in the Configuration Register.
 * This function should be used with QSPI memories not supporting the Read
 * Configuration Register (35h) instruction.
 *
 * bit 1 of the Configuration Register is the QE bit for Spansion like QSPI
 * memories.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int sr2_bit1_no_read_quad_enable(struct spi_nor *nor)
{
	u8 sr_cr[2];
	int ret;

	/* Keep the current value of the Status Register. */
	ret = read_sr(nor);
	if (ret < 0) {
		dev_err(nor->dev, "error while reading status register\n");
		return -EINVAL;
	}
	sr_cr[0] = ret;
	sr_cr[1] = CR_QUAD_EN_SPAN;

	return write_sr_cr(nor, sr_cr);
}

/**
 * sr2_bit1_read_quad_enable() - set QE bit in Configuration Register.
 * @nor:	pointer to a 'struct spi_nor'
 *
 * Set the Quad Enable (QE) bit in the Configuration Register.
 * This function should be used with QSPI memories supporting the Read
 * Configuration Register (35h) instruction.
 *
 * bit 1 of the Configuration Register is the QE bit for Spansion like QSPI
 * memories.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int sr2_bit1_read_quad_enable(struct spi_nor *nor)
{
	struct device *dev = nor->dev;
	u8 sr_cr[2];
	int ret;

	/* Check current Quad Enable bit value. */
	ret = read_cr(nor);
	if (ret < 0) {
		dev_err(dev, "error while reading configuration register\n");
		return -EINVAL;
	}

	if (ret & CR_QUAD_EN_SPAN)
		return 0;

	sr_cr[1] = ret | CR_QUAD_EN_SPAN;

	/* Keep the current value of the Status Register. */
	ret = read_sr(nor);
	if (ret < 0) {
		dev_err(dev, "error while reading status register\n");
		return -EINVAL;
	}
	sr_cr[0] = ret;

	ret = write_sr_cr(nor, sr_cr);
	if (ret)
		return ret;

	/* Read back and check it. */
	ret = read_cr(nor);
	if (!(ret > 0 && (ret & CR_QUAD_EN_SPAN))) {
		dev_err(nor->dev, "Spansion Quad bit not set\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * sr2_bit7_quad_enable() - set QE bit in Status Register 2.
 * @nor:	pointer to a 'struct spi_nor'
 *
 * Set the Quad Enable (QE) bit in the Status Register 2.
 *
 * This is one of the procedures to set the QE bit described in the SFDP
 * (JESD216 rev B) specification but no manufacturer using this procedure has
 * been identified yet, hence the name of the function.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int sr2_bit7_quad_enable(struct spi_nor *nor)
{
	u8 sr2;
	int ret;

	/* Check current Quad Enable bit value. */
	ret = nor->read_reg(nor, SPINOR_OP_RDSR2, &sr2, 1);
	if (ret)
		return ret;
	if (sr2 & SR2_QUAD_EN_BIT7)
		return 0;

	/* Update the Quad Enable bit. */
	sr2 |= SR2_QUAD_EN_BIT7;

	write_enable(nor);

	ret = nor->write_reg(nor, SPINOR_OP_WRSR2, &sr2, 1);
	if (ret < 0) {
		dev_err(nor->dev, "error while writing status register 2\n");
		return -EINVAL;
	}

	ret = spi_nor_wait_till_ready(nor);
	if (ret < 0) {
		dev_err(nor->dev, "timeout while writing status register 2\n");
		return ret;
	}

	/* Read back and check it. */
	ret = nor->read_reg(nor, SPINOR_OP_RDSR2, &sr2, 1);
	if (!(ret > 0 && (sr2 & SR2_QUAD_EN_BIT7))) {
		dev_err(nor->dev, "SR2 Quad bit not set\n");
		return -EINVAL;
	}

	return 0;
}

/* NOTE: double check command sets and memory organization when you add
 * more nor chips.  This current list focusses on newer chips, which
 * have been converging on command sets which including JEDEC ID.
 *
 * All newly added entries should describe *hardware* and should use SECT_4K
 * (or SECT_4K_PMC) if hardware supports erasing 4 KiB sectors. For usage
 * scenarios excluding small sectors there is config option that can be
 * disabled: CONFIG_MTD_SPI_NOR_USE_4K_SECTORS.
 * For historical (and compatibility) reasons (before we got above config) some
 * old entries may be missing 4K flag.
 */
static const struct flash_info spi_nor_ids[] = {
	/* Winbond -- w25x "blocks" are 64K, "sectors" are 4KiB */
	{ "w25x05", INFO(0xef3010, 0, 64 * 1024,  1,  SECT_4K) },
	{ "w25x10", INFO(0xef3011, 0, 64 * 1024,  2,  SECT_4K) },
	{ "w25x20", INFO(0xef3012, 0, 64 * 1024,  4,  SECT_4K) },
	{ "w25x40", INFO(0xef3013, 0, 64 * 1024,  8,  SECT_4K) },
	{ "w25x80", INFO(0xef3014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25x16", INFO(0xef3015, 0, 64 * 1024,  32, SECT_4K) },
	{
		"w25q16dw", INFO(0xef6015, 0, 64 * 1024,  32,
			SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
			SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{ "w25x32", INFO(0xef3016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25q20cl", INFO(0xef4012, 0, 64 * 1024,  4, SECT_4K) },
	{ "w25q20bw", INFO(0xef5012, 0, 64 * 1024,  4, SECT_4K) },
	{ "w25q20ew", INFO(0xef6012, 0, 64 * 1024,  4, SECT_4K) },
	{ "w25q32", INFO(0xef4016, 0, 64 * 1024,  64, SECT_4K) },
	{
		"w25q32dw", INFO(0xef6016, 0, 64 * 1024,  64,
			SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
			SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"w25q32jv", INFO(0xef7016, 0, 64 * 1024,  64,
			SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
			SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{ "w25x64", INFO(0xef3017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q64", INFO(0xef4017, 0, 64 * 1024, 128, SECT_4K) },
	{
		"w25q64dw", INFO(0xef6017, 0, 64 * 1024, 128,
			SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
			SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"w25q128fw", INFO(0xef6018, 0, 64 * 1024, 256,
			SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
			SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{
		"w25q128jv", INFO(0xef7018, 0, 64 * 1024, 256,
			SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ |
			SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB)
	},
	{ "w25q80", INFO(0xef5014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25q80bl", INFO(0xef4014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25q128", INFO(0xef4018, 0, 64 * 1024, 256, SECT_4K) },
	{ "w25q256", INFO(0xef4019, 0, 64 * 1024, 512, SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ) },
	{ "w25m512jv", INFO(0xef7119, 0, 64 * 1024, 1024,
			SECT_4K | SPI_NOR_QUAD_READ | SPI_NOR_DUAL_READ) },

	/* Catalyst / On Semiconductor -- non-JEDEC */
	{ "cat25c11", CAT25_INFO(  16, 8, 16, 1, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25c03", CAT25_INFO(  32, 8, 16, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25c09", CAT25_INFO( 128, 8, 32, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25c17", CAT25_INFO( 256, 8, 32, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25128", CAT25_INFO(2048, 8, 64, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },

	/* Xilinx S3AN Internal Flash */
	{ "3S50AN", S3AN_INFO(0x1f2200, 64, 264) },
	{ "3S200AN", S3AN_INFO(0x1f2400, 256, 264) },
	{ "3S400AN", S3AN_INFO(0x1f2400, 256, 264) },
	{ "3S700AN", S3AN_INFO(0x1f2500, 512, 264) },
	{ "3S1400AN", S3AN_INFO(0x1f2600, 512, 528) },

	/* XMC (Wuhan Xinxin Semiconductor Manufacturing Corp.) */
	{ "XM25QH64A", INFO(0x207017, 0, 64 * 1024, 128, SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ) },
	{ "XM25QH128A", INFO(0x207018, 0, 64 * 1024, 256, SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ) },
	{ },
};

static const struct spi_nor_manufacturer *manufacturers[] = {
	&spi_nor_atmel,
	&spi_nor_eon,
	&spi_nor_esmt,
	&spi_nor_everspin,
	&spi_nor_fujitsu,
	&spi_nor_gigadevice,
	&spi_nor_intel,
	&spi_nor_issi,
	&spi_nor_macronix,
	&spi_nor_micron,
	&spi_nor_spansion,
	&spi_nor_sst,
	&spi_nor_st,
};

static const struct flash_info *
spi_nor_search_part_by_id(const struct flash_info *parts, unsigned int nparts,
			  const u8 *id)
{
	unsigned int i;

	for (i = 0; i < nparts; i++) {
		if (parts[i].id_len &&
		    !memcmp(parts[i].id, id, parts[i].id_len))
			return &parts[i];
	}

	return NULL;
}

static const struct flash_info *spi_nor_read_id(struct spi_nor *nor)
{
	int			tmp;
	u8			id[SPI_NOR_MAX_ID_LEN];
	const struct flash_info	*info;

	tmp = nor->read_reg(nor, SPINOR_OP_RDID, id, SPI_NOR_MAX_ID_LEN);
	if (tmp < 0) {
		dev_dbg(nor->dev, "error %d reading JEDEC ID\n", tmp);
		return ERR_PTR(tmp);
	}

	for (tmp = 0; tmp < ARRAY_SIZE(manufacturers); tmp++) {
		info = spi_nor_search_part_by_id(manufacturers[tmp]->parts,
						 manufacturers[tmp]->nparts,
						 id);
		if (info) {
			nor->manufacturer = manufacturers[tmp];
			return info;
		}
	}

	info = spi_nor_search_part_by_id(spi_nor_ids,
					 ARRAY_SIZE(spi_nor_ids) - 1, id);
	if (info)
		return info;

	dev_err(nor->dev, "unrecognized JEDEC id bytes: %02x, %02x, %02x\n",
		id[0], id[1], id[2]);
	return ERR_PTR(-ENODEV);
}

static int spi_nor_read(struct mtd_info *mtd, loff_t from, size_t len,
			size_t *retlen, u_char *buf)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	int ret;

	dev_dbg(nor->dev, "from 0x%08x, len %zd\n", (u32)from, len);

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_READ);
	if (ret)
		return ret;

	while (len) {
		loff_t addr = from;

		addr = spi_nor_convert_addr(nor, addr);

		ret = nor->read(nor, addr, len, buf);
		if (ret == 0) {
			/* We shouldn't see 0-length reads */
			ret = -EIO;
			goto read_err;
		}
		if (ret < 0)
			goto read_err;

		WARN_ON(ret > len);
		*retlen += ret;
		buf += ret;
		from += ret;
		len -= ret;
	}
	ret = 0;

read_err:
	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_READ);
	return ret;
}

/*
 * Write an address range to the nor chip.  Data must be written in
 * FLASH_PAGESIZE chunks.  The address range may be any size provided
 * it is within the physical boundaries.
 */
static int spi_nor_write(struct mtd_info *mtd, loff_t to, size_t len,
	size_t *retlen, const u_char *buf)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	size_t page_offset, page_remain, i;
	ssize_t ret;

	dev_dbg(nor->dev, "to 0x%08x, len %zd\n", (u32)to, len);

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_WRITE);
	if (ret)
		return ret;

	for (i = 0; i < len; ) {
		ssize_t written;
		loff_t addr = to + i;

		/*
		 * If page_size is a power of two, the offset can be quickly
		 * calculated with an AND operation. On the other cases we
		 * need to do a modulus operation (more expensive).
		 * Power of two numbers have only one bit set and we can use
		 * the instruction hweight32 to detect if we need to do a
		 * modulus (do_div()) or not.
		 */
		if (hweight32(nor->page_size) == 1) {
			page_offset = addr & (nor->page_size - 1);
		} else {
			uint64_t aux = addr;

			page_offset = do_div(aux, nor->page_size);
		}
		/* the size of data remaining on the first page */
		page_remain = min_t(size_t,
				    nor->page_size - page_offset, len - i);

		addr = spi_nor_convert_addr(nor, addr);

		write_enable(nor);
		ret = nor->write(nor, addr, page_remain, buf + i);
		if (ret < 0)
			goto write_err;
		written = ret;

		ret = spi_nor_wait_till_ready(nor);
		if (ret)
			goto write_err;
		*retlen += written;
		i += written;
	}

write_err:
	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_WRITE);
	return ret;
}

static int spi_nor_check(struct spi_nor *nor)
{
	if (!nor->dev || !nor->read || !nor->write ||
		!nor->read_reg || !nor->write_reg) {
		pr_err("spi-nor: please fill all the necessary fields!\n");
		return -EINVAL;
	}

	return 0;
}

static void
spi_nor_set_read_settings(struct spi_nor_read_command *read,
			  u8 num_mode_clocks,
			  u8 num_wait_states,
			  u8 opcode,
			  enum spi_nor_protocol proto)
{
	read->num_mode_clocks = num_mode_clocks;
	read->num_wait_states = num_wait_states;
	read->opcode = opcode;
	read->proto = proto;
}

static void
spi_nor_set_pp_settings(struct spi_nor_pp_command *pp,
			u8 opcode,
			enum spi_nor_protocol proto)
{
	pp->opcode = opcode;
	pp->proto = proto;
}

static int spi_nor_hwcaps2cmd(u32 hwcaps, const int table[][2], size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (table[i][0] == (int)hwcaps)
			return table[i][1];

	return -EINVAL;
}

static int spi_nor_hwcaps_read2cmd(u32 hwcaps)
{
	static const int hwcaps_read2cmd[][2] = {
		{ SNOR_HWCAPS_READ,		SNOR_CMD_READ },
		{ SNOR_HWCAPS_READ_FAST,	SNOR_CMD_READ_FAST },
		{ SNOR_HWCAPS_READ_1_1_1_DTR,	SNOR_CMD_READ_1_1_1_DTR },
		{ SNOR_HWCAPS_READ_1_1_2,	SNOR_CMD_READ_1_1_2 },
		{ SNOR_HWCAPS_READ_1_2_2,	SNOR_CMD_READ_1_2_2 },
		{ SNOR_HWCAPS_READ_2_2_2,	SNOR_CMD_READ_2_2_2 },
		{ SNOR_HWCAPS_READ_1_2_2_DTR,	SNOR_CMD_READ_1_2_2_DTR },
		{ SNOR_HWCAPS_READ_1_1_4,	SNOR_CMD_READ_1_1_4 },
		{ SNOR_HWCAPS_READ_1_4_4,	SNOR_CMD_READ_1_4_4 },
		{ SNOR_HWCAPS_READ_4_4_4,	SNOR_CMD_READ_4_4_4 },
		{ SNOR_HWCAPS_READ_1_4_4_DTR,	SNOR_CMD_READ_1_4_4_DTR },
		{ SNOR_HWCAPS_READ_1_1_8,	SNOR_CMD_READ_1_1_8 },
		{ SNOR_HWCAPS_READ_1_8_8,	SNOR_CMD_READ_1_8_8 },
		{ SNOR_HWCAPS_READ_8_8_8,	SNOR_CMD_READ_8_8_8 },
		{ SNOR_HWCAPS_READ_1_8_8_DTR,	SNOR_CMD_READ_1_8_8_DTR },
	};

	return spi_nor_hwcaps2cmd(hwcaps, hwcaps_read2cmd,
				  ARRAY_SIZE(hwcaps_read2cmd));
}

static int spi_nor_hwcaps_pp2cmd(u32 hwcaps)
{
	static const int hwcaps_pp2cmd[][2] = {
		{ SNOR_HWCAPS_PP,		SNOR_CMD_PP },
		{ SNOR_HWCAPS_PP_1_1_4,		SNOR_CMD_PP_1_1_4 },
		{ SNOR_HWCAPS_PP_1_4_4,		SNOR_CMD_PP_1_4_4 },
		{ SNOR_HWCAPS_PP_4_4_4,		SNOR_CMD_PP_4_4_4 },
		{ SNOR_HWCAPS_PP_1_1_8,		SNOR_CMD_PP_1_1_8 },
		{ SNOR_HWCAPS_PP_1_8_8,		SNOR_CMD_PP_1_8_8 },
		{ SNOR_HWCAPS_PP_8_8_8,		SNOR_CMD_PP_8_8_8 },
	};

	return spi_nor_hwcaps2cmd(hwcaps, hwcaps_pp2cmd,
				  ARRAY_SIZE(hwcaps_pp2cmd));
}

/*
 * Serial Flash Discoverable Parameters (SFDP) parsing.
 */

/**
 * spi_nor_read_raw() - raw read of serial flash memory. read_opcode,
 *			addr_width and read_dummy members of the struct spi_nor
 *			should be previously
 * set.
 * @nor:	pointer to a 'struct spi_nor'
 * @addr:	offset in the serial flash memory
 * @len:	number of bytes to read
 * @buf:	buffer where the data is copied into
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_read_raw(struct spi_nor *nor, u32 addr, size_t len, u8 *buf)
{
	int ret;

	while (len) {
		ret = nor->read(nor, addr, len, buf);
		if (!ret || ret > len)
			return -EIO;
		if (ret < 0)
			return ret;

		buf += ret;
		addr += ret;
		len -= ret;
	}
	return 0;
}

/**
 * spi_nor_read_sfdp() - read Serial Flash Discoverable Parameters.
 * @nor:	pointer to a 'struct spi_nor'
 * @addr:	offset in the SFDP area to start reading data from
 * @len:	number of bytes to read
 * @buf:	buffer where the SFDP data are copied into (dma-safe memory)
 *
 * Whatever the actual numbers of bytes for address and dummy cycles are
 * for (Fast) Read commands, the Read SFDP (5Ah) instruction is always
 * followed by a 3-byte address and 8 dummy clock cycles.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_read_sfdp(struct spi_nor *nor, u32 addr,
			     size_t len, void *buf)
{
	u8 addr_width, read_opcode, read_dummy;
	int ret;

	read_opcode = nor->read_opcode;
	addr_width = nor->addr_width;
	read_dummy = nor->read_dummy;

	nor->read_opcode = SPINOR_OP_RDSFDP;
	nor->addr_width = 3;
	nor->read_dummy = 8;

	ret = spi_nor_read_raw(nor, addr, len, buf);

	nor->read_opcode = read_opcode;
	nor->addr_width = addr_width;
	nor->read_dummy = read_dummy;

	return ret;
}

/**
 * spi_nor_read_sfdp_dma_unsafe() - read Serial Flash Discoverable Parameters.
 * @nor:	pointer to a 'struct spi_nor'
 * @addr:	offset in the SFDP area to start reading data from
 * @len:	number of bytes to read
 * @buf:	buffer where the SFDP data are copied into
 *
 * Wrap spi_nor_read_sfdp() using a kmalloc'ed bounce buffer as @buf is now not
 * guaranteed to be dma-safe.
 *
 * Return: -ENOMEM if kmalloc() fails, the return code of spi_nor_read_sfdp()
 *          otherwise.
 */
static int spi_nor_read_sfdp_dma_unsafe(struct spi_nor *nor, u32 addr,
					size_t len, void *buf)
{
	void *dma_safe_buf;
	int ret;

	dma_safe_buf = kmalloc(len, GFP_KERNEL);
	if (!dma_safe_buf)
		return -ENOMEM;

	ret = spi_nor_read_sfdp(nor, addr, len, dma_safe_buf);
	memcpy(buf, dma_safe_buf, len);
	kfree(dma_safe_buf);

	return ret;
}

/* Fast Read settings. */

static void
spi_nor_set_read_settings_from_bfpt(struct spi_nor_read_command *read,
				    u16 half,
				    enum spi_nor_protocol proto)
{
	read->num_mode_clocks = (half >> 5) & 0x07;
	read->num_wait_states = (half >> 0) & 0x1f;
	read->opcode = (half >> 8) & 0xff;
	read->proto = proto;
}

struct sfdp_bfpt_read {
	/* The Fast Read x-y-z hardware capability in params->hwcaps.mask. */
	u32			hwcaps;

	/*
	 * The <supported_bit> bit in <supported_dword> BFPT DWORD tells us
	 * whether the Fast Read x-y-z command is supported.
	 */
	u32			supported_dword;
	u32			supported_bit;

	/*
	 * The half-word at offset <setting_shift> in <setting_dword> BFPT DWORD
	 * encodes the op code, the number of mode clocks and the number of wait
	 * states to be used by Fast Read x-y-z command.
	 */
	u32			settings_dword;
	u32			settings_shift;

	/* The SPI protocol for this Fast Read x-y-z command. */
	enum spi_nor_protocol	proto;
};

static const struct sfdp_bfpt_read sfdp_bfpt_reads[] = {
	/* Fast Read 1-1-2 */
	{
		SNOR_HWCAPS_READ_1_1_2,
		BFPT_DWORD(1), BIT(16),	/* Supported bit */
		BFPT_DWORD(4), 0,	/* Settings */
		SNOR_PROTO_1_1_2,
	},

	/* Fast Read 1-2-2 */
	{
		SNOR_HWCAPS_READ_1_2_2,
		BFPT_DWORD(1), BIT(20),	/* Supported bit */
		BFPT_DWORD(4), 16,	/* Settings */
		SNOR_PROTO_1_2_2,
	},

	/* Fast Read 2-2-2 */
	{
		SNOR_HWCAPS_READ_2_2_2,
		BFPT_DWORD(5),  BIT(0),	/* Supported bit */
		BFPT_DWORD(6), 16,	/* Settings */
		SNOR_PROTO_2_2_2,
	},

	/* Fast Read 1-1-4 */
	{
		SNOR_HWCAPS_READ_1_1_4,
		BFPT_DWORD(1), BIT(22),	/* Supported bit */
		BFPT_DWORD(3), 16,	/* Settings */
		SNOR_PROTO_1_1_4,
	},

	/* Fast Read 1-4-4 */
	{
		SNOR_HWCAPS_READ_1_4_4,
		BFPT_DWORD(1), BIT(21),	/* Supported bit */
		BFPT_DWORD(3), 0,	/* Settings */
		SNOR_PROTO_1_4_4,
	},

	/* Fast Read 4-4-4 */
	{
		SNOR_HWCAPS_READ_4_4_4,
		BFPT_DWORD(5), BIT(4),	/* Supported bit */
		BFPT_DWORD(7), 16,	/* Settings */
		SNOR_PROTO_4_4_4,
	},
};

struct sfdp_bfpt_erase {
	/*
	 * The half-word at offset <shift> in DWORD <dwoard> encodes the
	 * op code and erase sector size to be used by Sector Erase commands.
	 */
	u32			dword;
	u32			shift;
};

static const struct sfdp_bfpt_erase sfdp_bfpt_erases[] = {
	/* Erase Type 1 in DWORD8 bits[15:0] */
	{BFPT_DWORD(8), 0},

	/* Erase Type 2 in DWORD8 bits[31:16] */
	{BFPT_DWORD(8), 16},

	/* Erase Type 3 in DWORD9 bits[15:0] */
	{BFPT_DWORD(9), 0},

	/* Erase Type 4 in DWORD9 bits[31:16] */
	{BFPT_DWORD(9), 16},
};

/**
 * spi_nor_set_erase_type() - set a SPI NOR erase type
 * @erase:	pointer to a structure that describes a SPI NOR erase type
 * @size:	the size of the sector/block erased by the erase type
 * @opcode:	the SPI command op code to erase the sector/block
 */
static void spi_nor_set_erase_type(struct spi_nor_erase_type *erase,
				   u32 size, u8 opcode)
{
	erase->size = size;
	erase->opcode = opcode;
	/* JEDEC JESD216B Standard imposes erase sizes to be power of 2. */
	erase->size_shift = ffs(erase->size) - 1;
	erase->size_mask = (1 << erase->size_shift) - 1;
}

/**
 * spi_nor_set_erase_settings_from_bfpt() - set erase type settings from BFPT
 * @erase:	pointer to a structure that describes a SPI NOR erase type
 * @size:	the size of the sector/block erased by the erase type
 * @opcode:	the SPI command op code to erase the sector/block
 * @i:		erase type index as sorted in the Basic Flash Parameter Table
 *
 * The supported Erase Types will be sorted at init in ascending order, with
 * the smallest Erase Type size being the first member in the erase_type array
 * of the spi_nor_erase_map structure. Save the Erase Type index as sorted in
 * the Basic Flash Parameter Table since it will be used later on to
 * synchronize with the supported Erase Types defined in SFDP optional tables.
 */
static void
spi_nor_set_erase_settings_from_bfpt(struct spi_nor_erase_type *erase,
				     u32 size, u8 opcode, u8 i)
{
	erase->idx = i;
	spi_nor_set_erase_type(erase, size, opcode);
}

/**
 * spi_nor_map_cmp_erase_type() - compare the map's erase types by size
 * @l:	member in the left half of the map's erase_type array
 * @r:	member in the right half of the map's erase_type array
 *
 * Comparison function used in the sort() call to sort in ascending order the
 * map's erase types, the smallest erase type size being the first member in the
 * sorted erase_type array.
 *
 * Return: the result of @l->size - @r->size
 */
static int spi_nor_map_cmp_erase_type(const void *l, const void *r)
{
	const struct spi_nor_erase_type *left = l, *right = r;

	return left->size - right->size;
}

/**
 * spi_nor_regions_sort_erase_types() - sort erase types in each region
 * @map:	the erase map of the SPI NOR
 *
 * Function assumes that the erase types defined in the erase map are already
 * sorted in ascending order, with the smallest erase type size being the first
 * member in the erase_type array. It replicates the sort done for the map's
 * erase types. Each region's erase bitmask will indicate which erase types are
 * supported from the sorted erase types defined in the erase map.
 * Sort the all region's erase type at init in order to speed up the process of
 * finding the best erase command at runtime.
 */
static void spi_nor_regions_sort_erase_types(struct spi_nor_erase_map *map)
{
	struct spi_nor_erase_region *region = map->regions;
	struct spi_nor_erase_type *erase_type = map->erase_type;
	int i;
	u8 region_erase_mask, sorted_erase_mask;

	while (region) {
		region_erase_mask = region->offset & SNOR_ERASE_TYPE_MASK;

		/* Replicate the sort done for the map's erase types. */
		sorted_erase_mask = 0;
		for (i = 0; i < SNOR_ERASE_TYPE_MAX; i++)
			if (erase_type[i].size &&
			    region_erase_mask & BIT(erase_type[i].idx))
				sorted_erase_mask |= BIT(i);

		/* Overwrite erase mask. */
		region->offset = (region->offset & ~SNOR_ERASE_TYPE_MASK) |
				 sorted_erase_mask;

		region = spi_nor_region_next(region);
	}
}

/**
 * spi_nor_init_uniform_erase_map() - Initialize uniform erase map
 * @map:		the erase map of the SPI NOR
 * @erase_mask:		bitmask encoding erase types that can erase the entire
 *			flash memory
 * @flash_size:		the spi nor flash memory size
 */
static void spi_nor_init_uniform_erase_map(struct spi_nor_erase_map *map,
					   u8 erase_mask, u64 flash_size)
{
	/* Offset 0 with erase_mask and SNOR_LAST_REGION bit set */
	map->uniform_region.offset = (erase_mask & SNOR_ERASE_TYPE_MASK) |
				     SNOR_LAST_REGION;
	map->uniform_region.size = flash_size;
	map->regions = &map->uniform_region;
	map->uniform_erase_type = erase_mask;
}

static int
spi_nor_post_bfpt_fixups(struct spi_nor *nor,
			 const struct sfdp_parameter_header *bfpt_header,
			 const struct sfdp_bfpt *bfpt,
			 struct spi_nor_flash_parameter *params)
{
	int ret;

	if (nor->manufacturer && nor->manufacturer->fixups &&
	    nor->manufacturer->fixups->post_bfpt) {
		ret = nor->manufacturer->fixups->post_bfpt(nor, bfpt_header,
							   bfpt, params);
		if (ret)
			return ret;
	}

	if (nor->info->fixups && nor->info->fixups->post_bfpt) {
		ret = nor->info->fixups->post_bfpt(nor, bfpt_header, bfpt,
						   params);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * spi_nor_parse_bfpt() - read and parse the Basic Flash Parameter Table.
 * @nor:		pointer to a 'struct spi_nor'
 * @bfpt_header:	pointer to the 'struct sfdp_parameter_header' describing
 *			the Basic Flash Parameter Table length and version
 * @params:		pointer to the 'struct spi_nor_flash_parameter' to be
 *			filled
 *
 * The Basic Flash Parameter Table is the main and only mandatory table as
 * defined by the SFDP (JESD216) specification.
 * It provides us with the total size (memory density) of the data array and
 * the number of address bytes for Fast Read, Page Program and Sector Erase
 * commands.
 * For Fast READ commands, it also gives the number of mode clock cycles and
 * wait states (regrouped in the number of dummy clock cycles) for each
 * supported instruction op code.
 * For Page Program, the page size is now available since JESD216 rev A, however
 * the supported instruction op codes are still not provided.
 * For Sector Erase commands, this table stores the supported instruction op
 * codes and the associated sector sizes.
 * Finally, the Quad Enable Requirements (QER) are also available since JESD216
 * rev A. The QER bits encode the manufacturer dependent procedure to be
 * executed to set the Quad Enable (QE) bit in some internal register of the
 * Quad SPI memory. Indeed the QE bit, when it exists, must be set before
 * sending any Quad SPI command to the memory. Actually, setting the QE bit
 * tells the memory to reassign its WP# and HOLD#/RESET# pins to functions IO2
 * and IO3 hence enabling 4 (Quad) I/O lines.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_parse_bfpt(struct spi_nor *nor,
			      const struct sfdp_parameter_header *bfpt_header,
			      struct spi_nor_flash_parameter *params)
{
	struct spi_nor_erase_map *map = &nor->erase_map;
	struct spi_nor_erase_type *erase_type = map->erase_type;
	struct sfdp_bfpt bfpt;
	size_t len;
	int i, cmd, err;
	u32 addr;
	u16 half;
	u8 erase_mask;

	/* JESD216 Basic Flash Parameter Table length is at least 9 DWORDs. */
	if (bfpt_header->length < BFPT_DWORD_MAX_JESD216)
		return -EINVAL;

	/* Read the Basic Flash Parameter Table. */
	len = min_t(size_t, sizeof(bfpt),
		    bfpt_header->length * sizeof(u32));
	addr = SFDP_PARAM_HEADER_PTP(bfpt_header);
	memset(&bfpt, 0, sizeof(bfpt));
	err = spi_nor_read_sfdp_dma_unsafe(nor,  addr, len, &bfpt);
	if (err < 0)
		return err;

	/* Fix endianness of the BFPT DWORDs. */
	for (i = 0; i < BFPT_DWORD_MAX; i++)
		bfpt.dwords[i] = le32_to_cpu(bfpt.dwords[i]);

	/* Number of address bytes. */
	switch (bfpt.dwords[BFPT_DWORD(1)] & BFPT_DWORD1_ADDRESS_BYTES_MASK) {
	case BFPT_DWORD1_ADDRESS_BYTES_3_ONLY:
		nor->addr_width = 3;
		break;

	case BFPT_DWORD1_ADDRESS_BYTES_4_ONLY:
		nor->addr_width = 4;
		break;

	default:
		break;
	}

	/* Flash Memory Density (in bits). */
	params->size = bfpt.dwords[BFPT_DWORD(2)];
	if (params->size & BIT(31)) {
		params->size &= ~BIT(31);

		/*
		 * Prevent overflows on params->size. Anyway, a NOR of 2^64
		 * bits is unlikely to exist so this error probably means
		 * the BFPT we are reading is corrupted/wrong.
		 */
		if (params->size > 63)
			return -EINVAL;

		params->size = 1ULL << params->size;
	} else {
		params->size++;
	}
	params->size >>= 3; /* Convert to bytes. */

	/* Fast Read settings. */
	for (i = 0; i < ARRAY_SIZE(sfdp_bfpt_reads); i++) {
		const struct sfdp_bfpt_read *rd = &sfdp_bfpt_reads[i];
		struct spi_nor_read_command *read;

		if (!(bfpt.dwords[rd->supported_dword] & rd->supported_bit)) {
			params->hwcaps.mask &= ~rd->hwcaps;
			continue;
		}

		params->hwcaps.mask |= rd->hwcaps;
		cmd = spi_nor_hwcaps_read2cmd(rd->hwcaps);
		read = &params->reads[cmd];
		half = bfpt.dwords[rd->settings_dword] >> rd->settings_shift;
		spi_nor_set_read_settings_from_bfpt(read, half, rd->proto);
	}

	/*
	 * Sector Erase settings. Reinitialize the uniform erase map using the
	 * Erase Types defined in the bfpt table.
	 */
	erase_mask = 0;
	memset(&nor->erase_map, 0, sizeof(nor->erase_map));
	for (i = 0; i < ARRAY_SIZE(sfdp_bfpt_erases); i++) {
		const struct sfdp_bfpt_erase *er = &sfdp_bfpt_erases[i];
		u32 erasesize;
		u8 opcode;

		half = bfpt.dwords[er->dword] >> er->shift;
		erasesize = half & 0xff;

		/* erasesize == 0 means this Erase Type is not supported. */
		if (!erasesize)
			continue;

		erasesize = 1U << erasesize;
		opcode = (half >> 8) & 0xff;
		erase_mask |= BIT(i);
		spi_nor_set_erase_settings_from_bfpt(&erase_type[i], erasesize,
						     opcode, i);
	}
	spi_nor_init_uniform_erase_map(map, erase_mask, params->size);
	/*
	 * Sort all the map's Erase Types in ascending order with the smallest
	 * erase size being the first member in the erase_type array.
	 */
	sort(erase_type, SNOR_ERASE_TYPE_MAX, sizeof(erase_type[0]),
	     spi_nor_map_cmp_erase_type, NULL);
	/*
	 * Sort the erase types in the uniform region in order to update the
	 * uniform_erase_type bitmask. The bitmask will be used later on when
	 * selecting the uniform erase.
	 */
	spi_nor_regions_sort_erase_types(map);
	map->uniform_erase_type = map->uniform_region.offset &
				  SNOR_ERASE_TYPE_MASK;

	/* Stop here if not JESD216 rev A or later. */
	if (bfpt_header->length < BFPT_DWORD_MAX)
		return spi_nor_post_bfpt_fixups(nor, bfpt_header, &bfpt,
						params);

	/* Page size: this field specifies 'N' so the page size = 2^N bytes. */
	params->page_size = bfpt.dwords[BFPT_DWORD(11)];
	params->page_size &= BFPT_DWORD11_PAGE_SIZE_MASK;
	params->page_size >>= BFPT_DWORD11_PAGE_SIZE_SHIFT;
	params->page_size = 1U << params->page_size;

	/* Quad Enable Requirements. */
	switch (bfpt.dwords[BFPT_DWORD(15)] & BFPT_DWORD15_QER_MASK) {
	case BFPT_DWORD15_QER_NONE:
		params->quad_enable = no_quad_enable;
		break;

	case BFPT_DWORD15_QER_SR2_BIT1_BUGGY:
	case BFPT_DWORD15_QER_SR2_BIT1_NO_RD:
		params->quad_enable = sr2_bit1_no_read_quad_enable;
		break;

	case BFPT_DWORD15_QER_SR1_BIT6:
		params->quad_enable = sr1_bit6_quad_enable;
		break;

	case BFPT_DWORD15_QER_SR2_BIT7:
		params->quad_enable = sr2_bit7_quad_enable;
		break;

	case BFPT_DWORD15_QER_SR2_BIT1:
		params->quad_enable = sr2_bit1_read_quad_enable;
		break;

	default:
		return -EINVAL;
	}

	return spi_nor_post_bfpt_fixups(nor, bfpt_header, &bfpt, params);
}

#define SMPT_CMD_ADDRESS_LEN_MASK		GENMASK(23, 22)
#define SMPT_CMD_ADDRESS_LEN_0			(0x0UL << 22)
#define SMPT_CMD_ADDRESS_LEN_3			(0x1UL << 22)
#define SMPT_CMD_ADDRESS_LEN_4			(0x2UL << 22)
#define SMPT_CMD_ADDRESS_LEN_USE_CURRENT	(0x3UL << 22)

#define SMPT_CMD_READ_DUMMY_MASK		GENMASK(19, 16)
#define SMPT_CMD_READ_DUMMY_SHIFT		16
#define SMPT_CMD_READ_DUMMY(_cmd) \
	(((_cmd) & SMPT_CMD_READ_DUMMY_MASK) >> SMPT_CMD_READ_DUMMY_SHIFT)
#define SMPT_CMD_READ_DUMMY_IS_VARIABLE		0xfUL

#define SMPT_CMD_READ_DATA_MASK			GENMASK(31, 24)
#define SMPT_CMD_READ_DATA_SHIFT		24
#define SMPT_CMD_READ_DATA(_cmd) \
	(((_cmd) & SMPT_CMD_READ_DATA_MASK) >> SMPT_CMD_READ_DATA_SHIFT)

#define SMPT_CMD_OPCODE_MASK			GENMASK(15, 8)
#define SMPT_CMD_OPCODE_SHIFT			8
#define SMPT_CMD_OPCODE(_cmd) \
	(((_cmd) & SMPT_CMD_OPCODE_MASK) >> SMPT_CMD_OPCODE_SHIFT)

#define SMPT_MAP_REGION_COUNT_MASK		GENMASK(23, 16)
#define SMPT_MAP_REGION_COUNT_SHIFT		16
#define SMPT_MAP_REGION_COUNT(_header) \
	((((_header) & SMPT_MAP_REGION_COUNT_MASK) >> \
	  SMPT_MAP_REGION_COUNT_SHIFT) + 1)

#define SMPT_MAP_ID_MASK			GENMASK(15, 8)
#define SMPT_MAP_ID_SHIFT			8
#define SMPT_MAP_ID(_header) \
	(((_header) & SMPT_MAP_ID_MASK) >> SMPT_MAP_ID_SHIFT)

#define SMPT_MAP_REGION_SIZE_MASK		GENMASK(31, 8)
#define SMPT_MAP_REGION_SIZE_SHIFT		8
#define SMPT_MAP_REGION_SIZE(_region) \
	(((((_region) & SMPT_MAP_REGION_SIZE_MASK) >> \
	   SMPT_MAP_REGION_SIZE_SHIFT) + 1) * 256)

#define SMPT_MAP_REGION_ERASE_TYPE_MASK		GENMASK(3, 0)
#define SMPT_MAP_REGION_ERASE_TYPE(_region) \
	((_region) & SMPT_MAP_REGION_ERASE_TYPE_MASK)

#define SMPT_DESC_TYPE_MAP			BIT(1)
#define SMPT_DESC_END				BIT(0)

/**
 * spi_nor_smpt_addr_width() - return the address width used in the
 *			       configuration detection command.
 * @nor:	pointer to a 'struct spi_nor'
 * @settings:	configuration detection command descriptor, dword1
 */
static u8 spi_nor_smpt_addr_width(const struct spi_nor *nor, const u32 settings)
{
	switch (settings & SMPT_CMD_ADDRESS_LEN_MASK) {
	case SMPT_CMD_ADDRESS_LEN_0:
		return 0;
	case SMPT_CMD_ADDRESS_LEN_3:
		return 3;
	case SMPT_CMD_ADDRESS_LEN_4:
		return 4;
	case SMPT_CMD_ADDRESS_LEN_USE_CURRENT:
		/* fall through */
	default:
		return nor->addr_width;
	}
}

/**
 * spi_nor_smpt_read_dummy() - return the configuration detection command read
 *			       latency, in clock cycles.
 * @nor:	pointer to a 'struct spi_nor'
 * @settings:	configuration detection command descriptor, dword1
 *
 * Return: the number of dummy cycles for an SMPT read
 */
static u8 spi_nor_smpt_read_dummy(const struct spi_nor *nor, const u32 settings)
{
	u8 read_dummy = SMPT_CMD_READ_DUMMY(settings);

	if (read_dummy == SMPT_CMD_READ_DUMMY_IS_VARIABLE)
		return nor->read_dummy;
	return read_dummy;
}

/**
 * spi_nor_get_map_in_use() - get the configuration map in use
 * @nor:	pointer to a 'struct spi_nor'
 * @smpt:	pointer to the sector map parameter table
 */
static const u32 *spi_nor_get_map_in_use(struct spi_nor *nor, const u32 *smpt)
{
	const u32 *ret = NULL;
	u32 i, addr;
	int err;
	u8 addr_width, read_opcode, read_dummy;
	u8 read_data_mask, data_byte, map_id;

	addr_width = nor->addr_width;
	read_dummy = nor->read_dummy;
	read_opcode = nor->read_opcode;

	map_id = 0;
	i = 0;
	/* Determine if there are any optional Detection Command Descriptors */
	while (!(smpt[i] & SMPT_DESC_TYPE_MAP)) {
		read_data_mask = SMPT_CMD_READ_DATA(smpt[i]);
		nor->addr_width = spi_nor_smpt_addr_width(nor, smpt[i]);
		nor->read_dummy = spi_nor_smpt_read_dummy(nor, smpt[i]);
		nor->read_opcode = SMPT_CMD_OPCODE(smpt[i]);
		addr = smpt[i + 1];

		err = spi_nor_read_raw(nor, addr, 1, &data_byte);
		if (err)
			goto out;

		/*
		 * Build an index value that is used to select the Sector Map
		 * Configuration that is currently in use.
		 */
		map_id = map_id << 1 | !!(data_byte & read_data_mask);
		i = i + 2;
	}

	/* Find the matching configuration map */
	while (SMPT_MAP_ID(smpt[i]) != map_id) {
		if (smpt[i] & SMPT_DESC_END)
			goto out;
		/* increment the table index to the next map */
		i += SMPT_MAP_REGION_COUNT(smpt[i]) + 1;
	}

	ret = smpt + i;
	/* fall through */
out:
	nor->addr_width = addr_width;
	nor->read_dummy = read_dummy;
	nor->read_opcode = read_opcode;
	return ret;
}

/**
 * spi_nor_region_check_overlay() - set overlay bit when the region is overlaid
 * @region:	pointer to a structure that describes a SPI NOR erase region
 * @erase:	pointer to a structure that describes a SPI NOR erase type
 * @erase_type:	erase type bitmask
 */
static void
spi_nor_region_check_overlay(struct spi_nor_erase_region *region,
			     const struct spi_nor_erase_type *erase,
			     const u8 erase_type)
{
	int i;

	for (i = 0; i < SNOR_ERASE_TYPE_MAX; i++) {
		if (!(erase_type & BIT(i)))
			continue;
		if (region->size & erase[i].size_mask) {
			spi_nor_region_mark_overlay(region);
			return;
		}
	}
}

/**
 * spi_nor_init_non_uniform_erase_map() - initialize the non-uniform erase map
 * @nor:	pointer to a 'struct spi_nor'
 * @smpt:	pointer to the sector map parameter table
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_init_non_uniform_erase_map(struct spi_nor *nor,
					      const u32 *smpt)
{
	struct spi_nor_erase_map *map = &nor->erase_map;
	const struct spi_nor_erase_type *erase = map->erase_type;
	struct spi_nor_erase_region *region;
	u64 offset;
	u32 region_count;
	int i, j;
	u8 erase_type;

	region_count = SMPT_MAP_REGION_COUNT(*smpt);
	/*
	 * The regions will be freed when the driver detaches from the
	 * device.
	 */
	region = devm_kcalloc(nor->dev, region_count, sizeof(*region),
			      GFP_KERNEL);
	if (!region)
		return -ENOMEM;
	map->regions = region;

	map->uniform_erase_type = 0xff;
	offset = 0;
	/* Populate regions. */
	for (i = 0; i < region_count; i++) {
		j = i + 1; /* index for the region dword */
		region[i].size = SMPT_MAP_REGION_SIZE(smpt[j]);
		erase_type = SMPT_MAP_REGION_ERASE_TYPE(smpt[j]);
		region[i].offset = offset | erase_type;

		spi_nor_region_check_overlay(&region[i], erase, erase_type);

		/*
		 * Save the erase types that are supported in all regions and
		 * can erase the entire flash memory.
		 */
		map->uniform_erase_type &= erase_type;

		offset = (region[i].offset & ~SNOR_ERASE_FLAGS_MASK) +
			 region[i].size;
	}

	spi_nor_region_mark_end(&region[i - 1]);

	return 0;
}

/**
 * spi_nor_parse_smpt() - parse Sector Map Parameter Table
 * @nor:		pointer to a 'struct spi_nor'
 * @smpt_header:	sector map parameter table header
 *
 * This table is optional, but when available, we parse it to identify the
 * location and size of sectors within the main data array of the flash memory
 * device and to identify which Erase Types are supported by each sector.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_parse_smpt(struct spi_nor *nor,
			      const struct sfdp_parameter_header *smpt_header)
{
	const u32 *sector_map;
	u32 *smpt;
	size_t len;
	u32 addr;
	int i, ret;

	/* Read the Sector Map Parameter Table. */
	len = smpt_header->length * sizeof(*smpt);
	smpt = kmalloc(len, GFP_KERNEL);
	if (!smpt)
		return -ENOMEM;

	addr = SFDP_PARAM_HEADER_PTP(smpt_header);
	ret = spi_nor_read_sfdp(nor, addr, len, smpt);
	if (ret)
		goto out;

	/* Fix endianness of the SMPT DWORDs. */
	for (i = 0; i < smpt_header->length; i++)
		smpt[i] = le32_to_cpu(smpt[i]);

	sector_map = spi_nor_get_map_in_use(nor, smpt);
	if (!sector_map) {
		ret = -EINVAL;
		goto out;
	}

	ret = spi_nor_init_non_uniform_erase_map(nor, sector_map);
	if (ret)
		goto out;

	spi_nor_regions_sort_erase_types(&nor->erase_map);
	/* fall through */
out:
	kfree(smpt);
	return ret;
}

/**
 * spi_nor_parse_sfdp() - parse the Serial Flash Discoverable Parameters.
 * @nor:		pointer to a 'struct spi_nor'
 * @params:		pointer to the 'struct spi_nor_flash_parameter' to be
 *			filled
 *
 * The Serial Flash Discoverable Parameters are described by the JEDEC JESD216
 * specification. This is a standard which tends to supported by almost all
 * (Q)SPI memory manufacturers. Those hard-coded tables allow us to learn at
 * runtime the main parameters needed to perform basic SPI flash operations such
 * as Fast Read, Page Program or Sector Erase commands.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int spi_nor_parse_sfdp(struct spi_nor *nor,
			      struct spi_nor_flash_parameter *params)
{
	const struct sfdp_parameter_header *param_header, *bfpt_header;
	struct sfdp_parameter_header *param_headers = NULL;
	struct sfdp_header header;
	struct device *dev = nor->dev;
	size_t psize;
	int i, err;

	/* Get the SFDP header. */
	err = spi_nor_read_sfdp_dma_unsafe(nor, 0, sizeof(header), &header);
	if (err < 0)
		return err;

	/* Check the SFDP header version. */
	if (le32_to_cpu(header.signature) != SFDP_SIGNATURE ||
	    header.major != SFDP_JESD216_MAJOR)
		return -EINVAL;

	/*
	 * Verify that the first and only mandatory parameter header is a
	 * Basic Flash Parameter Table header as specified in JESD216.
	 */
	bfpt_header = &header.bfpt_header;
	if (SFDP_PARAM_HEADER_ID(bfpt_header) != SFDP_BFPT_ID ||
	    bfpt_header->major != SFDP_JESD216_MAJOR)
		return -EINVAL;

	/*
	 * Allocate memory then read all parameter headers with a single
	 * Read SFDP command. These parameter headers will actually be parsed
	 * twice: a first time to get the latest revision of the basic flash
	 * parameter table, then a second time to handle the supported optional
	 * tables.
	 * Hence we read the parameter headers once for all to reduce the
	 * processing time. Also we use kmalloc() instead of devm_kmalloc()
	 * because we don't need to keep these parameter headers: the allocated
	 * memory is always released with kfree() before exiting this function.
	 */
	if (header.nph) {
		psize = header.nph * sizeof(*param_headers);

		param_headers = kmalloc(psize, GFP_KERNEL);
		if (!param_headers)
			return -ENOMEM;

		err = spi_nor_read_sfdp(nor, sizeof(header),
					psize, param_headers);
		if (err < 0) {
			dev_err(dev, "failed to read SFDP parameter headers\n");
			goto exit;
		}
	}

	/*
	 * Check other parameter headers to get the latest revision of
	 * the basic flash parameter table.
	 */
	for (i = 0; i < header.nph; i++) {
		param_header = &param_headers[i];

		if (SFDP_PARAM_HEADER_ID(param_header) == SFDP_BFPT_ID &&
		    param_header->major == SFDP_JESD216_MAJOR &&
		    (param_header->minor > bfpt_header->minor ||
		     (param_header->minor == bfpt_header->minor &&
		      param_header->length > bfpt_header->length)))
			bfpt_header = param_header;
	}

	err = spi_nor_parse_bfpt(nor, bfpt_header, params);
	if (err)
		goto exit;

	/* Parse other parameter headers. */
	for (i = 0; i < header.nph; i++) {
		param_header = &param_headers[i];

		switch (SFDP_PARAM_HEADER_ID(param_header)) {
		case SFDP_SECTOR_MAP_ID:
			err = spi_nor_parse_smpt(nor, param_header);
			break;

		default:
			break;
		}

		if (err)
			goto exit;
	}

exit:
	kfree(param_headers);
	return err;
}

static int spi_nor_init_params(struct spi_nor *nor,
			       struct spi_nor_flash_parameter *params)
{
	struct spi_nor_erase_map *map = &nor->erase_map;
	const struct flash_info *info = nor->info;
	u8 i, erase_mask;

	/* Set legacy flash parameters as default. */
	memset(params, 0, sizeof(*params));

	/* Set SPI NOR sizes. */
	params->size = (u64)info->sector_size * info->n_sectors;
	params->page_size = info->page_size;

	/* (Fast) Read settings. */
	params->hwcaps.mask |= SNOR_HWCAPS_READ;
	spi_nor_set_read_settings(&params->reads[SNOR_CMD_READ],
				  0, 0, SPINOR_OP_READ,
				  SNOR_PROTO_1_1_1);

	if (!(info->flags & SPI_NOR_NO_FR)) {
		params->hwcaps.mask |= SNOR_HWCAPS_READ_FAST;
		spi_nor_set_read_settings(&params->reads[SNOR_CMD_READ_FAST],
					  0, 8, SPINOR_OP_READ_FAST,
					  SNOR_PROTO_1_1_1);
	}

	if (info->flags & SPI_NOR_DUAL_READ) {
		params->hwcaps.mask |= SNOR_HWCAPS_READ_1_1_2;
		spi_nor_set_read_settings(&params->reads[SNOR_CMD_READ_1_1_2],
					  0, 8, SPINOR_OP_READ_1_1_2,
					  SNOR_PROTO_1_1_2);
	}

	if (info->flags & SPI_NOR_QUAD_READ) {
		params->hwcaps.mask |= SNOR_HWCAPS_READ_1_1_4;
		spi_nor_set_read_settings(&params->reads[SNOR_CMD_READ_1_1_4],
					  0, 8, SPINOR_OP_READ_1_1_4,
					  SNOR_PROTO_1_1_4);
	}

	/* Page Program settings. */
	params->hwcaps.mask |= SNOR_HWCAPS_PP;
	spi_nor_set_pp_settings(&params->page_programs[SNOR_CMD_PP],
				SPINOR_OP_PP, SNOR_PROTO_1_1_1);

	/*
	 * Sector Erase settings. Sort Erase Types in ascending order, with the
	 * smallest erase size starting at BIT(0).
	 */
	erase_mask = 0;
	i = 0;
	if (info->flags & SECT_4K_PMC) {
		erase_mask |= BIT(i);
		spi_nor_set_erase_type(&map->erase_type[i], 4096u,
				       SPINOR_OP_BE_4K_PMC);
		i++;
	} else if (info->flags & SECT_4K) {
		erase_mask |= BIT(i);
		spi_nor_set_erase_type(&map->erase_type[i], 4096u,
				       SPINOR_OP_BE_4K);
		i++;
	}
	erase_mask |= BIT(i);
	spi_nor_set_erase_type(&map->erase_type[i], info->sector_size,
			       SPINOR_OP_SE);
	spi_nor_init_uniform_erase_map(map, erase_mask, params->size);

	if ((info->flags & (SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)) &&
	    !(info->flags & SPI_NOR_SKIP_SFDP)) {
		struct spi_nor_flash_parameter sfdp_params;
		struct spi_nor_erase_map prev_map;

		memcpy(&sfdp_params, params, sizeof(sfdp_params));
		memcpy(&prev_map, &nor->erase_map, sizeof(prev_map));

		if (spi_nor_parse_sfdp(nor, &sfdp_params)) {
			nor->addr_width = 0;
			nor->flags &= ~SNOR_F_4B_OPCODES;
			/* restore previous erase map */
			memcpy(&nor->erase_map, &prev_map,
			       sizeof(nor->erase_map));
		} else {
			memcpy(params, &sfdp_params, sizeof(*params));
		}
	}

	return 0;
}

static int spi_nor_select_read(struct spi_nor *nor,
			       const struct spi_nor_flash_parameter *params,
			       u32 shared_hwcaps)
{
	int cmd, best_match = fls(shared_hwcaps & SNOR_HWCAPS_READ_MASK) - 1;
	const struct spi_nor_read_command *read;

	if (best_match < 0)
		return -EINVAL;

	cmd = spi_nor_hwcaps_read2cmd(BIT(best_match));
	if (cmd < 0)
		return -EINVAL;

	read = &params->reads[cmd];
	nor->read_opcode = read->opcode;
	nor->read_proto = read->proto;

	/*
	 * In the spi-nor framework, we don't need to make the difference
	 * between mode clock cycles and wait state clock cycles.
	 * Indeed, the value of the mode clock cycles is used by a QSPI
	 * flash memory to know whether it should enter or leave its 0-4-4
	 * (Continuous Read / XIP) mode.
	 * eXecution In Place is out of the scope of the mtd sub-system.
	 * Hence we choose to merge both mode and wait state clock cycles
	 * into the so called dummy clock cycles.
	 */
	nor->read_dummy = read->num_mode_clocks + read->num_wait_states;
	return 0;
}

static int spi_nor_select_pp(struct spi_nor *nor,
			     const struct spi_nor_flash_parameter *params,
			     u32 shared_hwcaps)
{
	int cmd, best_match = fls(shared_hwcaps & SNOR_HWCAPS_PP_MASK) - 1;
	const struct spi_nor_pp_command *pp;

	if (best_match < 0)
		return -EINVAL;

	cmd = spi_nor_hwcaps_pp2cmd(BIT(best_match));
	if (cmd < 0)
		return -EINVAL;

	pp = &params->page_programs[cmd];
	nor->program_opcode = pp->opcode;
	nor->write_proto = pp->proto;
	return 0;
}

/**
 * spi_nor_select_uniform_erase() - select optimum uniform erase type
 * @map:		the erase map of the SPI NOR
 * @wanted_size:	the erase type size to search for. Contains the value of
 *			info->sector_size or of the "small sector" size in case
 *			CONFIG_MTD_SPI_NOR_USE_4K_SECTORS is defined.
 *
 * Once the optimum uniform sector erase command is found, disable all the
 * other.
 *
 * Return: pointer to erase type on success, NULL otherwise.
 */
static const struct spi_nor_erase_type *
spi_nor_select_uniform_erase(struct spi_nor_erase_map *map,
			     const u32 wanted_size)
{
	const struct spi_nor_erase_type *tested_erase, *erase = NULL;
	int i;
	u8 uniform_erase_type = map->uniform_erase_type;

	for (i = SNOR_ERASE_TYPE_MAX - 1; i >= 0; i--) {
		if (!(uniform_erase_type & BIT(i)))
			continue;

		tested_erase = &map->erase_type[i];

		/*
		 * If the current erase size is the one, stop here:
		 * we have found the right uniform Sector Erase command.
		 */
		if (tested_erase->size == wanted_size) {
			erase = tested_erase;
			break;
		}

		/*
		 * Otherwise, the current erase size is still a valid canditate.
		 * Select the biggest valid candidate.
		 */
		if (!erase && tested_erase->size)
			erase = tested_erase;
			/* keep iterating to find the wanted_size */
	}

	if (!erase)
		return NULL;

	/* Disable all other Sector Erase commands. */
	map->uniform_erase_type &= ~SNOR_ERASE_TYPE_MASK;
	map->uniform_erase_type |= BIT(erase - map->erase_type);
	return erase;
}

static int spi_nor_select_erase(struct spi_nor *nor, u32 wanted_size)
{
	struct spi_nor_erase_map *map = &nor->erase_map;
	const struct spi_nor_erase_type *erase = NULL;
	struct mtd_info *mtd = &nor->mtd;
	int i;

	/*
	 * The previous implementation handling Sector Erase commands assumed
	 * that the SPI flash memory has an uniform layout then used only one
	 * of the supported erase sizes for all Sector Erase commands.
	 * So to be backward compatible, the new implementation also tries to
	 * manage the SPI flash memory as uniform with a single erase sector
	 * size, when possible.
	 */
#ifdef CONFIG_MTD_SPI_NOR_USE_4K_SECTORS
	/* prefer "small sector" erase if possible */
	wanted_size = 4096u;
#endif

	if (spi_nor_has_uniform_erase(nor)) {
		erase = spi_nor_select_uniform_erase(map, wanted_size);
		if (!erase)
			return -EINVAL;
		nor->erase_opcode = erase->opcode;
		mtd->erasesize = erase->size;
		return 0;
	}

	/*
	 * For non-uniform SPI flash memory, set mtd->erasesize to the
	 * maximum erase sector size. No need to set nor->erase_opcode.
	 */
	for (i = SNOR_ERASE_TYPE_MAX - 1; i >= 0; i--) {
		if (map->erase_type[i].size) {
			erase = &map->erase_type[i];
			break;
		}
	}

	if (!erase)
		return -EINVAL;

	mtd->erasesize = erase->size;
	return 0;
}

static int spi_nor_setup(struct spi_nor *nor,
			 const struct spi_nor_flash_parameter *params,
			 const struct spi_nor_hwcaps *hwcaps)
{
	u32 ignored_mask, shared_mask;
	int err;

	/*
	 * Some manufacturers select the opcode to use in their fixups()
	 * method, and explicitly ask to skip the generic selection logic.
	 */
	if (nor->flags & SNOR_F_SKIP_SETUP)
		return 0;

	/*
	 * Keep only the hardware capabilities supported by both the SPI
	 * controller and the SPI flash memory.
	 */
	shared_mask = hwcaps->mask & params->hwcaps.mask;

	/* SPI n-n-n protocols are not supported yet. */
	ignored_mask = (SNOR_HWCAPS_READ_2_2_2 |
			SNOR_HWCAPS_READ_4_4_4 |
			SNOR_HWCAPS_READ_8_8_8 |
			SNOR_HWCAPS_PP_4_4_4 |
			SNOR_HWCAPS_PP_8_8_8);
	if (shared_mask & ignored_mask) {
		dev_dbg(nor->dev,
			"SPI n-n-n protocols are not supported yet.\n");
		shared_mask &= ~ignored_mask;
	}

	/* Select the (Fast) Read command. */
	err = spi_nor_select_read(nor, params, shared_mask);
	if (err) {
		dev_err(nor->dev,
			"can't select read settings supported by both the SPI controller and memory.\n");
		return err;
	}

	/* Select the Page Program command. */
	err = spi_nor_select_pp(nor, params, shared_mask);
	if (err) {
		dev_err(nor->dev,
			"can't select write settings supported by both the SPI controller and memory.\n");
		return err;
	}

	/* Select the Sector Erase command. */
	err = spi_nor_select_erase(nor, nor->info->sector_size);
	if (err) {
		dev_err(nor->dev,
			"can't select erase settings supported by both the SPI controller and memory.\n");
		return err;
	}

	/* Enable Quad I/O if needed. */
	if (params->quad_enable)
		nor->quad_enable = params->quad_enable;

	return 0;
}

static int spi_nor_quad_enable(struct spi_nor *nor)
{
	if (spi_nor_get_protocol_width(nor->read_proto) != 4 &&
	    spi_nor_get_protocol_width(nor->write_proto) != 4)
		return 0;

	if (!nor->quad_enable)
		return 0;

	return nor->quad_enable(nor);
}

static int spi_nor_init(struct spi_nor *nor)
{
	int err;

	/*
	 * Atmel, SST, Intel/Numonyx, and others serial NOR tend to power up
	 * with the software protection bits set
	 */
	if (nor->flags & SNOR_F_CLR_SW_PROT_BITS) {
		write_enable(nor);
		write_sr(nor, 0);
		spi_nor_wait_till_ready(nor);
	}

	err = spi_nor_quad_enable(nor);
	if (err) {
		dev_err(nor->dev, "quad mode not supported\n");
		return err;
	}

	if (nor->addr_width == 4 && !(nor->flags & SNOR_F_4B_OPCODES)) {
		/*
		 * If the RESET# pin isn't hooked up properly, or the system
		 * otherwise doesn't perform a reset command in the boot
		 * sequence, it's impossible to 100% protect against unexpected
		 * reboots (e.g., crashes). Warn the user (or hopefully, system
		 * designer) that this is bad.
		 */
		WARN_ONCE(nor->flags & SNOR_F_BROKEN_RESET,
			  "enabling reset hack; may not recover from unexpected reboots\n");
		set_4byte(nor, true);
	}

	return 0;
}

/* mtd resume handler */
static void spi_nor_resume(struct mtd_info *mtd)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	struct device *dev = nor->dev;
	int ret;

	/* re-initialize the nor chip */
	ret = spi_nor_init(nor);
	if (ret)
		dev_err(dev, "resume() failed\n");
}

void spi_nor_restore(struct spi_nor *nor)
{
	/* restore the addressing mode */
	if (nor->addr_width == 4 && !(nor->flags & SNOR_F_4B_OPCODES) &&
	    nor->flags & SNOR_F_BROKEN_RESET)
		set_4byte(nor, false);
}
EXPORT_SYMBOL_GPL(spi_nor_restore);

int en4_ex4_set_4byte(struct spi_nor *nor, bool enable)
{
	return nor->write_reg(nor, enable ? SPINOR_OP_EN4B : SPINOR_OP_EX4B,
			      NULL, 0);
}

int en4_ex4_wen_set_4byte(struct spi_nor *nor, bool enable)
{
	int ret;

	write_enable(nor);
	ret = en4_ex4_set_4byte(nor, enable);
	write_disable(nor);

	return ret;
}

static int winbond_set_4byte(struct spi_nor *nor, bool enable)
{
	int ret;

	ret = en4_ex4_set_4byte(nor, enable);
	if (ret || enable)
		return ret;

	/*
	 * On Winbond W25Q256FV, leaving 4byte mode causes the Extended Address
	 * Register to be set to 1, so all 3-byte-address reads come from the
	 * second 16M.
	 * We must clear the register to enable normal behavior.
	 */
	write_enable(nor);
	nor->cmd_buf[0] = 0;
	nor->write_reg(nor, SPINOR_OP_WREAR, nor->cmd_buf, 1);
	write_disable(nor);

	return ret;
}

static void winbond_post_sfdp_fixups(struct spi_nor *nor)
{
	nor->set_4byte = winbond_set_4byte;
}

static int s3an_post_sfdp_fixups(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_XRDSR, &val, 1);
	if (ret < 0) {
		dev_err(nor->dev, "error %d reading XRDSR\n", (int) ret);
		return ret;
	}

	/*
	 * We choose the opcodes we want to use, so let's add
	 * SNOR_F_SKIP_SETUP to prevent spi_nor_setup() from changing
	 * them behind our back.
	 */
	nor->flags |= SNOR_F_SKIP_SETUP;
	nor->erase_opcode = SPINOR_OP_XSE;
	nor->program_opcode = SPINOR_OP_XPP;
	nor->read_opcode = SPINOR_OP_READ;
	nor->flags |= SNOR_F_NO_OP_CHIP_ERASE;

	/*
	 * This flashes have a page size of 264 or 528 bytes (known as
	 * Default addressing mode). It can be changed to a more standard
	 * Power of two mode where the page size is 256/512. This comes
	 * with a price: there is 3% less of space, the data is corrupted
	 * and the page size cannot be changed back to default addressing
	 * mode.
	 *
	 * The current addressing mode can be read from the XRDSR register
	 * and should not be changed, because is a destructive operation.
	 */
	if (val & XSR_PAGESIZE) {
		/* Flash in Power of 2 mode */
		nor->page_size = (nor->page_size == 264) ? 256 : 512;
		nor->mtd.writebufsize = nor->page_size;
		nor->mtd.size = 8 * nor->page_size * nor->info->n_sectors;
		nor->mtd.erasesize = 8 * nor->page_size;
	} else {
		/* Flash in Default addressing mode */
		nor->convert_addr = s3an_convert_addr;
	}

	return 0;
}


static int
spi_nor_manufacturer_post_sfdp_fixups(struct spi_nor *nor,
				      struct spi_nor_flash_parameter *params)
{
	if (nor->manufacturer && nor->manufacturer->fixups &&
	    nor->manufacturer->fixups->post_sfdp)
		return nor->manufacturer->fixups->post_sfdp(nor, params);

	switch (JEDEC_MFR(nor->info)) {
	case SNOR_MFR_WINBOND:
		winbond_post_sfdp_fixups(nor);
		break;

	default:
		break;
	}

	if (nor->info->flags & SPI_S3AN)
		return s3an_post_sfdp_fixups(nor);

	return 0;
}

static int spi_nor_post_sfdp_fixups(struct spi_nor *nor,
				    struct spi_nor_flash_parameter *params)
{
	int ret;

	ret = spi_nor_manufacturer_post_sfdp_fixups(nor, params);
	if (ret)
		return ret;

	if (nor->info->fixups && nor->info->fixups->post_sfdp)
		ret = nor->info->fixups->post_sfdp(nor, params);

	return ret;
}

static const struct flash_info *spi_nor_match_id(struct spi_nor *nor,
						 const char *name)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(spi_nor_ids) - 1; i++) {
		if (!strcmp(name, spi_nor_ids[i].name))
			return &spi_nor_ids[i];
	}

	for (i = 0; i < ARRAY_SIZE(manufacturers); i++) {
		for (j = 0; j < manufacturers[i]->nparts; j++) {
			if (!strcmp(name, manufacturers[i]->parts[j].name)) {
				nor->manufacturer = manufacturers[i];
				return &manufacturers[i]->parts[j];
			}
		}
	}

	return NULL;
}

int spi_nor_scan(struct spi_nor *nor, const char *name,
		 const struct spi_nor_hwcaps *hwcaps)
{
	struct spi_nor_flash_parameter params;
	const struct flash_info *info = NULL;
	struct device *dev = nor->dev;
	struct mtd_info *mtd = &nor->mtd;
	struct device_node *np = spi_nor_get_flash_node(nor);
	int ret;
	int i;

	ret = spi_nor_check(nor);
	if (ret)
		return ret;

	/* Reset SPI protocol for all commands. */
	nor->reg_proto = SNOR_PROTO_1_1_1;
	nor->read_proto = SNOR_PROTO_1_1_1;
	nor->write_proto = SNOR_PROTO_1_1_1;

	if (name)
		info = spi_nor_match_id(nor, name);
	/* Try to auto-detect if chip name wasn't specified or not found */
	if (!info)
		info = spi_nor_read_id(nor);
	if (IS_ERR_OR_NULL(info))
		return -ENOENT;

	/*
	 * If caller has specified name of flash model that can normally be
	 * detected using JEDEC, let's verify it.
	 */
	if (name && info->id_len) {
		const struct flash_info *jinfo;

		jinfo = spi_nor_read_id(nor);
		if (IS_ERR(jinfo)) {
			return PTR_ERR(jinfo);
		} else if (jinfo != info) {
			/*
			 * JEDEC knows better, so overwrite platform ID. We
			 * can't trust partitions any longer, but we'll let
			 * mtd apply them anyway, since some partitions may be
			 * marked read-only, and we don't want to lose that
			 * information, even if it's not 100% accurate.
			 */
			dev_warn(dev, "found %s, expected %s\n",
				 jinfo->name, info->name);
			info = jinfo;
		}
	}

	nor->info = info;

	mutex_init(&nor->lock);

	/*
	 * Make sure the XSR_RDY flag is set before calling
	 * spi_nor_wait_till_ready(). Xilinx S3AN share MFR
	 * with Atmel spi-nor
	 */
	if (info->flags & SPI_NOR_XSR_RDY)
		nor->flags |=  SNOR_F_READY_XSR_RDY;

	/* Parse the Serial Flash Discoverable Parameters table. */
	ret = spi_nor_init_params(nor, &params);
	if (ret)
		return ret;

	if (!mtd->name)
		mtd->name = dev_name(dev);
	mtd->priv = nor;
	mtd->type = MTD_NORFLASH;
	mtd->writesize = 1;
	mtd->flags = MTD_CAP_NORFLASH;
	mtd->size = params.size;
	mtd->_erase = spi_nor_erase;
	mtd->_read = spi_nor_read;
	mtd->_resume = spi_nor_resume;
	mtd->_write = spi_nor_write;

	if (info->flags & USE_FSR)
		nor->flags |= SNOR_F_USE_FSR;
	if (info->flags & SPI_NOR_HAS_TB)
		nor->flags |= SNOR_F_HAS_SR_TB;
	if (info->flags & NO_CHIP_ERASE)
		nor->flags |= SNOR_F_NO_OP_CHIP_ERASE;
	if (info->flags & USE_CLSR)
		nor->flags |= SNOR_F_USE_CLSR;

	if (info->flags & SPI_NOR_NO_ERASE)
		mtd->flags |= MTD_NO_ERASE;

	mtd->dev.parent = dev;
	nor->page_size = params.page_size;
	mtd->writebufsize = nor->page_size;

	if (np) {
		/* If we were instantiated by DT, use it */
		if (of_property_read_bool(np, "m25p,fast-read"))
			params.hwcaps.mask |= SNOR_HWCAPS_READ_FAST;
		else
			params.hwcaps.mask &= ~SNOR_HWCAPS_READ_FAST;
	} else {
		/* If we weren't instantiated by DT, default to fast-read */
		params.hwcaps.mask |= SNOR_HWCAPS_READ_FAST;
	}

	if (of_property_read_bool(np, "broken-flash-reset"))
		nor->flags |= SNOR_F_BROKEN_RESET;

	/* Some devices cannot do fast-read, no matter what DT tells us */
	if (info->flags & SPI_NOR_NO_FR)
		params.hwcaps.mask &= ~SNOR_HWCAPS_READ_FAST;

	if (info->flags & SPI_NOR_HAS_LOCK)
		nor->flags |= SNOR_F_HAS_LOCK;

	/* Kept only for backward compatibility purpose. */
	nor->quad_enable = legacy_quad_enable;

	/*
	 * Post SFDP fixups. Has to be called before spi_nor_setup() because
	 * some fixups might modify params that are then used by
	 * spi_nor_setup() to select the opcodes.
	 */
	ret = spi_nor_post_sfdp_fixups(nor, &params);
	if (ret) {
		dev_err(dev, "failed in the post-SFDP fixups (err %d)\n",
			ret);
		return ret;
	}

	/*
	 * NOR protection support. When locking_ops are not provided, we
	 * pick the default ones.
	 */
	if (nor->flags & SNOR_F_HAS_LOCK && !nor->locking_ops)
		nor->locking_ops = &sr_locking_ops;

	if (nor->locking_ops) {
		mtd->_lock = spi_nor_lock;
		mtd->_unlock = spi_nor_unlock;
		mtd->_is_locked = spi_nor_is_locked;
		nor->flags |= SNOR_F_CLR_SW_PROT_BITS;
	}

	/*
	 * Configure the SPI memory:
	 * - select op codes for (Fast) Read, Page Program and Sector Erase.
	 * - set the number of dummy cycles (mode cycles + wait states).
	 * - set the SPI protocols for register and memory accesses.
	 * - set the Quad Enable bit if needed (required by SPI x-y-4 protos).
	 */
	ret = spi_nor_setup(nor, &params, hwcaps);
	if (ret)
		return ret;

	if (nor->addr_width) {
		/* already configured from SFDP */
	} else if (info->addr_width) {
		nor->addr_width = info->addr_width;
	} else if (mtd->size > 0x1000000) {
		/* enable 4-byte addressing if the device exceeds 16MiB */
		nor->addr_width = 4;
	} else {
		nor->addr_width = 3;
	}

	if (info->flags & SPI_NOR_4B_OPCODES)
		nor->flags |= SNOR_F_4B_OPCODES;

	if (nor->addr_width == 4 && nor->flags & SNOR_F_4B_OPCODES)
		spi_nor_set_4byte_opcodes(nor);

	if (nor->addr_width > SPI_NOR_MAX_ADDR_WIDTH) {
		dev_err(dev, "address width is too large: %u\n",
			nor->addr_width);
		return -EINVAL;
	}

	/* Send all the required SPI flash commands to initialize device */
	ret = spi_nor_init(nor);
	if (ret)
		return ret;

	dev_info(dev, "%s (%lld Kbytes)\n", info->name,
			(long long)mtd->size >> 10);

	dev_dbg(dev,
		"mtd .name = %s, .size = 0x%llx (%lldMiB), "
		".erasesize = 0x%.8x (%uKiB) .numeraseregions = %d\n",
		mtd->name, (long long)mtd->size, (long long)(mtd->size >> 20),
		mtd->erasesize, mtd->erasesize / 1024, mtd->numeraseregions);

	if (mtd->numeraseregions)
		for (i = 0; i < mtd->numeraseregions; i++)
			dev_dbg(dev,
				"mtd.eraseregions[%d] = { .offset = 0x%llx, "
				".erasesize = 0x%.8x (%uKiB), "
				".numblocks = %d }\n",
				i, (long long)mtd->eraseregions[i].offset,
				mtd->eraseregions[i].erasesize,
				mtd->eraseregions[i].erasesize / 1024,
				mtd->eraseregions[i].numblocks);
	return 0;
}
EXPORT_SYMBOL_GPL(spi_nor_scan);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Huang Shijie <shijie8@gmail.com>");
MODULE_AUTHOR("Mike Lavender");
MODULE_DESCRIPTION("framework for SPI NOR");
