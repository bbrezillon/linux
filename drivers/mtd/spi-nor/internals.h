/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#ifndef __LINUX_MTD_SPI_NOR_INTERNALS_H
#define __LINUX_MTD_SPI_NOR_INTERNALS_H

#define SPI_NOR_MAX_ID_LEN      6

struct spi_nor_read_command {
	u8			num_mode_clocks;
	u8			num_wait_states;
	u8			opcode;
	enum spi_nor_protocol	proto;
};

struct spi_nor_pp_command {
	u8			opcode;
	enum spi_nor_protocol	proto;
};

enum spi_nor_read_command_index {
	SNOR_CMD_READ,
	SNOR_CMD_READ_FAST,
	SNOR_CMD_READ_1_1_1_DTR,

	/* Dual SPI */
	SNOR_CMD_READ_1_1_2,
	SNOR_CMD_READ_1_2_2,
	SNOR_CMD_READ_2_2_2,
	SNOR_CMD_READ_1_2_2_DTR,

	/* Quad SPI */
	SNOR_CMD_READ_1_1_4,
	SNOR_CMD_READ_1_4_4,
	SNOR_CMD_READ_4_4_4,
	SNOR_CMD_READ_1_4_4_DTR,

	/* Octo SPI */
	SNOR_CMD_READ_1_1_8,
	SNOR_CMD_READ_1_8_8,
	SNOR_CMD_READ_8_8_8,
	SNOR_CMD_READ_1_8_8_DTR,

	SNOR_CMD_READ_MAX
};

enum spi_nor_pp_command_index {
	SNOR_CMD_PP,

	/* Quad SPI */
	SNOR_CMD_PP_1_1_4,
	SNOR_CMD_PP_1_4_4,
	SNOR_CMD_PP_4_4_4,

	/* Octo SPI */
	SNOR_CMD_PP_1_1_8,
	SNOR_CMD_PP_1_8_8,
	SNOR_CMD_PP_8_8_8,

	SNOR_CMD_PP_MAX
};

struct spi_nor_flash_parameter {
	u64				size;
	u32				page_size;

	struct spi_nor_hwcaps		hwcaps;
	struct spi_nor_read_command	reads[SNOR_CMD_READ_MAX];
	struct spi_nor_pp_command	page_programs[SNOR_CMD_PP_MAX];

	int (*quad_enable)(struct spi_nor *nor);
};

struct sfdp_parameter_header {
	u8		id_lsb;
	u8		minor;
	u8		major;
	u8		length; /* in double words */
	u8		parameter_table_pointer[3]; /* byte address */
	u8		id_msb;
};

#define SFDP_PARAM_HEADER_ID(p)	(((p)->id_msb << 8) | (p)->id_lsb)
#define SFDP_PARAM_HEADER_PTP(p) \
	(((p)->parameter_table_pointer[2] << 16) | \
	 ((p)->parameter_table_pointer[1] <<  8) | \
	 ((p)->parameter_table_pointer[0] <<  0))

#define SFDP_BFPT_ID		0xff00	/* Basic Flash Parameter Table */
#define SFDP_SECTOR_MAP_ID	0xff81	/* Sector Map Table */

#define SFDP_SIGNATURE		0x50444653U
#define SFDP_JESD216_MAJOR	1
#define SFDP_JESD216_MINOR	0
#define SFDP_JESD216A_MINOR	5
#define SFDP_JESD216B_MINOR	6

struct sfdp_header {
	u32		signature; /* Ox50444653U <=> "SFDP" */
	u8		minor;
	u8		major;
	u8		nph; /* 0-base number of parameter headers */
	u8		unused;

	/* Basic Flash Parameter Table. */
	struct sfdp_parameter_header	bfpt_header;
};

/* Basic Flash Parameter Table */

/*
 * JESD216 rev B defines a Basic Flash Parameter Table of 16 DWORDs.
 * They are indexed from 1 but C arrays are indexed from 0.
 */
#define BFPT_DWORD(i)		((i) - 1)
#define BFPT_DWORD_MAX		16

/* The first version of JESB216 defined only 9 DWORDs. */
#define BFPT_DWORD_MAX_JESD216			9

/* 1st DWORD. */
#define BFPT_DWORD1_FAST_READ_1_1_2		BIT(16)
#define BFPT_DWORD1_ADDRESS_BYTES_MASK		GENMASK(18, 17)

#define BFPT_DWORD1_ADDRESS_BYTES_3_ONLY	(0x0UL << 17)
#define BFPT_DWORD1_ADDRESS_BYTES_3_OR_4	(0x1UL << 17)
#define BFPT_DWORD1_ADDRESS_BYTES_4_ONLY	(0x2UL << 17)
#define BFPT_DWORD1_DTR				BIT(19)
#define BFPT_DWORD1_FAST_READ_1_2_2		BIT(20)
#define BFPT_DWORD1_FAST_READ_1_4_4		BIT(21)
#define BFPT_DWORD1_FAST_READ_1_1_4		BIT(22)

/* 5th DWORD. */
#define BFPT_DWORD5_FAST_READ_2_2_2		BIT(0)
#define BFPT_DWORD5_FAST_READ_4_4_4		BIT(4)

/* 11th DWORD. */
#define BFPT_DWORD11_PAGE_SIZE_SHIFT		4
#define BFPT_DWORD11_PAGE_SIZE_MASK		GENMASK(7, 4)

/* 15th DWORD. */

/*
 * (from JESD216 rev B)
 * Quad Enable Requirements (QER):
 * - 000b: Device does not have a QE bit. Device detects 1-1-4 and 1-4-4
 *         reads based on instruction. DQ3/HOLD# functions are hold during
 *         instruction phase.
 * - 001b: QE is bit 1 of status register 2. It is set via Write Status with
 *         two data bytes where bit 1 of the second byte is one.
 *         [...]
 *         Writing only one byte to the status register has the side-effect of
 *         clearing status register 2, including the QE bit. The 100b code is
 *         used if writing one byte to the status register does not modify
 *         status register 2.
 * - 010b: QE is bit 6 of status register 1. It is set via Write Status with
 *         one data byte where bit 6 is one.
 *         [...]
 * - 011b: QE is bit 7 of status register 2. It is set via Write status
 *         register 2 instruction 3Eh with one data byte where bit 7 is one.
 *         [...]
 *         The status register 2 is read using instruction 3Fh.
 * - 100b: QE is bit 1 of status register 2. It is set via Write Status with
 *         two data bytes where bit 1 of the second byte is one.
 *         [...]
 *         In contrast to the 001b code, writing one byte to the status
 *         register does not modify status register 2.
 * - 101b: QE is bit 1 of status register 2. Status register 1 is read using
 *         Read Status instruction 05h. Status register2 is read using
 *         instruction 35h. QE is set via Writ Status instruction 01h with
 *         two data bytes where bit 1 of the second byte is one.
 *         [...]
 */
#define BFPT_DWORD15_QER_MASK			GENMASK(22, 20)
#define BFPT_DWORD15_QER_NONE			(0x0UL << 20) /* Micron */
#define BFPT_DWORD15_QER_SR2_BIT1_BUGGY		(0x1UL << 20)
#define BFPT_DWORD15_QER_SR1_BIT6		(0x2UL << 20) /* Macronix */
#define BFPT_DWORD15_QER_SR2_BIT7		(0x3UL << 20)
#define BFPT_DWORD15_QER_SR2_BIT1_NO_RD		(0x4UL << 20)
#define BFPT_DWORD15_QER_SR2_BIT1		(0x5UL << 20) /* Spansion */

struct sfdp_bfpt {
	u32	dwords[BFPT_DWORD_MAX];
};

/**
 * struct spi_nor_fixups - SPI NOR fixup hooks
 * @post_bfpt: called after the BFPT table has been parsed
 * @post_sfdp: called after SFDP has been parsed (is also called for SPI NORs
 *	       that do not support RDSFDP). Typically used to tweak various
 *	       parameters that could not be extracted by other means (i.e.
 *	       when information provided by the SFDP/flash_info tables are
 *	       incomplete or wrong).
 *
 * Those hooks can be used to tweak the SPI NOR configuration when the SFDP
 * table is broken or not available.
 */
struct spi_nor_fixups {
	int (*post_bfpt)(struct spi_nor *nor,
			 const struct sfdp_parameter_header *bfpt_header,
			 const struct sfdp_bfpt *bfpt,
			 struct spi_nor_flash_parameter *params);
	int (*post_sfdp)(struct spi_nor *nor,
			 struct spi_nor_flash_parameter *params);
};

struct flash_info {
	char		*name;

	/*
	 * This array stores the ID bytes.
	 * The first three bytes are the JEDIC ID.
	 * JEDEC ID zero means "no ID" (mostly older chips).
	 */
	u8		id[SPI_NOR_MAX_ID_LEN];
	u8		id_len;

	/* The size listed here is what works with SPINOR_OP_SE, which isn't
	 * necessarily called a "sector" by the vendor.
	 */
	unsigned	sector_size;
	u16		n_sectors;

	u16		page_size;
	u16		addr_width;

	u16		flags;
#define SECT_4K			BIT(0)	/* SPINOR_OP_BE_4K works uniformly */
#define SPI_NOR_NO_ERASE	BIT(1)	/* No erase command needed */
#define SST_WRITE		BIT(2)	/* use SST byte programming */
#define SPI_NOR_NO_FR		BIT(3)	/* Can't do fastread */
#define SECT_4K_PMC		BIT(4)	/* SPINOR_OP_BE_4K_PMC works uniformly */
#define SPI_NOR_DUAL_READ	BIT(5)	/* Flash supports Dual Read */
#define SPI_NOR_QUAD_READ	BIT(6)	/* Flash supports Quad Read */
#define USE_FSR			BIT(7)	/* use flag status register */
#define SPI_NOR_HAS_LOCK	BIT(8)	/* Flash supports lock/unlock via SR */
#define SPI_NOR_HAS_TB		BIT(9)	/*
					 * Flash SR has Top/Bottom (TB) protect
					 * bit. Must be used with
					 * SPI_NOR_HAS_LOCK.
					 */
#define SPI_NOR_XSR_RDY		BIT(10)	/*
					 * S3AN flashes have specific opcode to
					 * read the status register.
					 * Flags SPI_NOR_XSR_RDY and SPI_S3AN
					 * use the same bit as one implies the
					 * other, but we will get rid of
					 * SPI_S3AN soon.
					 */
#define	SPI_S3AN		BIT(10)	/*
					 * Xilinx Spartan 3AN In-System Flash
					 * (MFR cannot be used for probing
					 * because it has the same value as
					 * ATMEL flashes)
					 */
#define SPI_NOR_4B_OPCODES	BIT(11)	/*
					 * Use dedicated 4byte address op codes
					 * to support memory size above 128Mib.
					 */
#define NO_CHIP_ERASE		BIT(12) /* Chip does not support chip erase */
#define SPI_NOR_SKIP_SFDP	BIT(13)	/* Skip parsing of SFDP tables */
#define USE_CLSR		BIT(14)	/* use CLSR command */

	/* Part specific fixup hooks. */
	const struct spi_nor_fixups *fixups;
};

/* Used when the "_ext_id" is two bytes at most */
#define INFO(_jedec_id, _ext_id, _sector_size, _n_sectors, _flags)	\
		.id = {							\
			((_jedec_id) >> 16) & 0xff,			\
			((_jedec_id) >> 8) & 0xff,			\
			(_jedec_id) & 0xff,				\
			((_ext_id) >> 8) & 0xff,			\
			(_ext_id) & 0xff,				\
			},						\
		.id_len = (!(_jedec_id) ? 0 : (3 + ((_ext_id) ? 2 : 0))),	\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = 256,					\
		.flags = (_flags),

#define INFO6(_jedec_id, _ext_id, _sector_size, _n_sectors, _flags)	\
		.id = {							\
			((_jedec_id) >> 16) & 0xff,			\
			((_jedec_id) >> 8) & 0xff,			\
			(_jedec_id) & 0xff,				\
			((_ext_id) >> 16) & 0xff,			\
			((_ext_id) >> 8) & 0xff,			\
			(_ext_id) & 0xff,				\
			},						\
		.id_len = 6,						\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = 256,					\
		.flags = (_flags),

#define CAT25_INFO(_sector_size, _n_sectors, _page_size, _addr_width, _flags)	\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = (_page_size),				\
		.addr_width = (_addr_width),				\
		.flags = (_flags),

#define S3AN_INFO(_jedec_id, _n_sectors, _page_size)			\
		.id = {							\
			((_jedec_id) >> 16) & 0xff,			\
			((_jedec_id) >> 8) & 0xff,			\
			(_jedec_id) & 0xff				\
			},						\
		.id_len = 3,						\
		.sector_size = (8*_page_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = _page_size,				\
		.addr_width = 3,					\
		.flags = SPI_NOR_NO_FR | SPI_S3AN,

/**
 * struct spi_nor_manufacturer - SPI NOR manufacturer object
 * @name: manufacturer name
 * @parts: array of parts supported by this manufacturer
 * @nparts: number of entries in the parts array
 * @fixups: hooks called at various points in time during spi_nor_scan()
 */
struct spi_nor_manufacturer {
	const char *name;
	const struct flash_info *parts;
	unsigned int nparts;
	const struct spi_nor_fixups *fixups;
};

/* Manufacturer drivers. */
extern const struct spi_nor_manufacturer spi_nor_atmel;
extern const struct spi_nor_manufacturer spi_nor_eon;
extern const struct spi_nor_manufacturer spi_nor_esmt;
extern const struct spi_nor_manufacturer spi_nor_everspin;
extern const struct spi_nor_manufacturer spi_nor_fujitsu;
extern const struct spi_nor_manufacturer spi_nor_gigadevice;
extern const struct spi_nor_manufacturer spi_nor_intel;
extern const struct spi_nor_manufacturer spi_nor_issi;
extern const struct spi_nor_manufacturer spi_nor_macronix;
extern const struct spi_nor_manufacturer spi_nor_micron;
extern const struct spi_nor_manufacturer spi_nor_spansion;
extern const struct spi_nor_manufacturer spi_nor_sst;
extern const struct spi_nor_manufacturer spi_nor_st;
extern const struct spi_nor_manufacturer spi_nor_winbond;

/* Core helpers. */
int en4_ex4_set_4byte(struct spi_nor *nor, bool enable);
int en4_ex4_wen_set_4byte(struct spi_nor *nor, bool enable);
int sr1_bit6_quad_enable(struct spi_nor *nor);
int no_quad_enable(struct spi_nor *nor);
int write_enable(struct spi_nor *nor);
int write_disable(struct spi_nor *nor);
int spi_nor_lock_and_prep(struct spi_nor *nor, enum spi_nor_ops ops);
void spi_nor_unlock_and_unprep(struct spi_nor *nor, enum spi_nor_ops ops);
int spi_nor_wait_till_ready(struct spi_nor *nor);

static inline struct spi_nor *mtd_to_spi_nor(struct mtd_info *mtd)
{
	return mtd->priv;
}

#endif /* __LINUX_MTD_SPI_NOR_INTERNALS_H */
