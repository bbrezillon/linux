/*
 * Copyright (c) International Business Machines Corp., 2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Author: Artem Bityutskiy (Битюцкий Артём)
 */

/*
 * UBI attaching sub-system.
 *
 * This sub-system is responsible for attaching MTD devices and it also
 * implements flash media scanning.
 *
 * The attaching information is represented by a &struct ubi_attach_info'
 * object. Information about volumes is represented by &struct ubi_ainf_volume
 * objects which are kept in volume RB-tree with root at the @volumes field.
 * The RB-tree is indexed by the volume ID.
 *
 * Logical eraseblocks are represented by &struct ubi_ainf_peb objects. These
 * objects are kept in per-volume RB-trees with the root at the corresponding
 * &struct ubi_ainf_volume object. To put it differently, we keep an RB-tree of
 * per-volume objects and each of these objects is the root of RB-tree of
 * per-LEB objects.
 *
 * Corrupted physical eraseblocks are put to the @corr list, free physical
 * eraseblocks are put to the @free list and the physical eraseblock to be
 * erased are put to the @erase list.
 *
 * About corruptions
 * ~~~~~~~~~~~~~~~~~
 *
 * UBI protects EC and VID headers with CRC-32 checksums, so it can detect
 * whether the headers are corrupted or not. Sometimes UBI also protects the
 * data with CRC-32, e.g., when it executes the atomic LEB change operation, or
 * when it moves the contents of a PEB for wear-leveling purposes.
 *
 * UBI tries to distinguish between 2 types of corruptions.
 *
 * 1. Corruptions caused by power cuts. These are expected corruptions and UBI
 * tries to handle them gracefully, without printing too many warnings and
 * error messages. The idea is that we do not lose important data in these
 * cases - we may lose only the data which were being written to the media just
 * before the power cut happened, and the upper layers (e.g., UBIFS) are
 * supposed to handle such data losses (e.g., by using the FS journal).
 *
 * When UBI detects a corruption (CRC-32 mismatch) in a PEB, and it looks like
 * the reason is a power cut, UBI puts this PEB to the @erase list, and all
 * PEBs in the @erase list are scheduled for erasure later.
 *
 * 2. Unexpected corruptions which are not caused by power cuts. During
 * attaching, such PEBs are put to the @corr list and UBI preserves them.
 * Obviously, this lessens the amount of available PEBs, and if at some  point
 * UBI runs out of free PEBs, it switches to R/O mode. UBI also loudly informs
 * about such PEBs every time the MTD device is attached.
 *
 * However, it is difficult to reliably distinguish between these types of
 * corruptions and UBI's strategy is as follows (in case of attaching by
 * scanning). UBI assumes corruption type 2 if the VID header is corrupted and
 * the data area does not contain all 0xFFs, and there were no bit-flips or
 * integrity errors (e.g., ECC errors in case of NAND) while reading the data
 * area.  Otherwise UBI assumes corruption type 1. So the decision criteria
 * are as follows.
 *   o If the data area contains only 0xFFs, there are no data, and it is safe
 *     to just erase this PEB - this is corruption type 1.
 *   o If the data area has bit-flips or data integrity errors (ECC errors on
 *     NAND), it is probably a PEB which was being erased when power cut
 *     happened, so this is corruption type 1. However, this is just a guess,
 *     which might be wrong.
 *   o Otherwise this is corruption type 2.
 */

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/math64.h>
#include <linux/random.h>
#include "ubi.h"

static int self_check_ai(struct ubi_device *ubi, struct ubi_attach_info *ai);

#define AV_FIND		BIT(0)
#define AV_ADD		BIT(1)
#define AV_FIND_OR_ADD	(AV_FIND | AV_ADD)

/**
 * find_or_add_av - internal function to find a volume, add a volume or do
 *		    both (find and add if missing).
 * @ai: attaching information
 * @vol_id: the requested volume ID
 * @flags: a combination of the %AV_FIND and %AV_ADD flags describing the
 *	   expected operation. If only %AV_ADD is set, -EEXIST is returned
 *	   if the volume already exists. If only %AV_FIND is set, NULL is
 *	   returned if the volume does not exist. And if both flags are
 *	   set, the helper first tries to find an existing volume, and if
 *	   it does not exist it creates a new one.
 * @created: in value used to inform the caller whether it"s a newly created
 *	     volume or not.
 *
 * This function returns a pointer to a volume description or an ERR_PTR if
 * the operation failed. It can also return NULL if only %AV_FIND is set and
 * the volume does not exist.
 */
static struct ubi_ainf_volume *find_or_add_av(struct ubi_attach_info *ai,
					      int vol_id, unsigned int flags,
					      bool *created)
{
	struct ubi_ainf_volume *av;
	struct rb_node **p = &ai->volumes.rb_node, *parent = NULL;

	/* Walk the volume RB-tree to look if this volume is already present */
	while (*p) {
		parent = *p;
		av = rb_entry(parent, struct ubi_ainf_volume, rb);

		if (vol_id == av->vol_id) {
			*created = false;

			if (!(flags & AV_FIND))
				return ERR_PTR(-EEXIST);

			return av;
		}

		if (vol_id > av->vol_id)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	if (!(flags & AV_ADD))
		return NULL;

	/* The volume is absent - add it */
	av = kzalloc(sizeof(*av), GFP_KERNEL);
	if (!av)
		return ERR_PTR(-ENOMEM);

	av->vol_id = vol_id;
	av->vol_mode = -1;

	if (vol_id > ai->highest_vol_id)
		ai->highest_vol_id = vol_id;

	rb_link_node(&av->rb, parent, p);
	rb_insert_color(&av->rb, &ai->volumes);
	ai->vols_found += 1;
	*created = true;
	dbg_bld("added volume %d", vol_id);
	return av;
}

/**
 * ubi_find_or_add_av - search for a volume in the attaching information and
 *			add one if it does not exist.
 * @ai: attaching information
 * @vol_id: the requested volume ID
 * @created: whether the volume has been created or not
 *
 * This function returns a pointer to the new volume description or an
 * ERR_PTR if the operation failed.
 */
static struct ubi_ainf_volume *ubi_find_or_add_av(struct ubi_attach_info *ai,
						  int vol_id, bool *created)
{
	return find_or_add_av(ai, vol_id, AV_FIND_OR_ADD, created);
}

/**
 * ubi_alloc_apeb - allocate an apeb element
 * @ai: attaching information
 * @pnum: physical eraseblock number
 * @ec: erase counter of the physical eraseblock
 *
 * Allocate an apeb object and initialize the pnum and ec information.
 * vol_id is set to UBI_UNKNOWN, and the other fields are initialized
 * to zero.
 * Note that the element is not added in any list.
 */
struct ubi_ainf_peb *ubi_alloc_apeb(struct ubi_attach_info *ai, int pnum,
				    int ec)
{
	struct ubi_ainf_peb *apeb;

	apeb = kmem_cache_zalloc(ai->apeb_slab_cache, GFP_KERNEL);
	if (!apeb)
		return NULL;

	apeb->sleb.pnum = pnum;
	apeb->ec = ec;
	apeb->vol_id = UBI_UNKNOWN;
	INIT_LIST_HEAD(&apeb->node);

	return apeb;
}

/**
 * ubi_free_apeb - free an apeb element
 * @ai: attaching information
 * @apeb: the element to free
 *
 * Free an apeb object. The caller must have removed the element from the list.
 */
void ubi_free_apeb(struct ubi_attach_info *ai, struct ubi_ainf_peb *apeb)
{
	if (apeb->consolidated && apeb->mleb.cpeb) {
		kfree(apeb->mleb.cpeb);
	}

	kmem_cache_free(ai->apeb_slab_cache, apeb);
}

/**
 * ubi_apeb_to_pdesc - create an PEB descriptor from an apeb element
 * @ubi: the UBI device
 * @apeb: the element containing attach information
 *
 * Returns a PEB descriptor or NULL if the allocation failed.
 */
struct ubi_peb_desc *ubi_apeb_to_pdesc(struct ubi_device *ubi,
				       const struct ubi_ainf_peb *apeb)
{
	struct ubi_peb_desc *pdesc;

	pdesc = ubi_alloc_pdesc(ubi, GFP_KERNEL);
	if (!pdesc)
		return NULL;

	pdesc->vol_id = apeb->vol_id;

	if (apeb->consolidated) {
		struct ubi_consolidated_peb *cpeb = apeb->mleb.cpeb;
		int i;

		ubi_assert(cpeb);

		pdesc->pnum = cpeb->pnum;
		for (i = 0; i < ubi->max_lebs_per_peb; i++)
			pdesc->lnums[i] = cpeb->lnums[i];
	} else {
		pdesc->pnum = apeb->sleb.pnum;
		pdesc->lnums[0] = apeb->sleb.lnum;
	}

	return pdesc;
}

/**
 * add_apeb_to_list - add physical eraseblock to a list.
 * @ai: attaching information
 * @apeb: physical eraseblock number to add
 * @to_head: if not zero, add to the head of the list
 * @list: the list to add to
 *
 * This function adds an already allocated peb to the "free", "erase", or
 * "alien" lists.
 * If @to_head is not zero, PEB will be added to the head of the list, which
 * basically means it will be processed first later. E.g., we add corrupted
 * PEBs (corrupted due to power cuts) to the head of the erase list to make
 * sure we erase them first and get rid of corruptions ASAP. This function
 * returns zero in case of success and a negative error code in case of
 * failure.
 */
static void add_apeb_to_list(struct ubi_attach_info *ai,
			     struct ubi_ainf_peb *apeb, int to_head,
			     struct list_head *list)
{
	int pnum = ubi_ainf_get_pnum(apeb);

	if (list == &ai->free) {
		dbg_bld("add to free: PEB %d, EC %d", pnum, apeb->ec);
	} else if (list == &ai->erase) {
		dbg_bld("add to erase: PEB %d, EC %d", pnum, apeb->ec);
	} else if (list == &ai->alien) {
		dbg_bld("add to alien: PEB %d, EC %d", pnum, apeb->ec);
		ai->alien_peb_count += 1;
	} else
		BUG();

	if (to_head)
		list_add(&apeb->node, list);
	else
		list_add_tail(&apeb->node, list);
}

/**
 * ubi_alloc_aleb - allocate an aleb element
 * @ai: attaching information
 * @apeb: PEB this LEB is attached to
 * @lnum: logical eraseblock number
 * @copy_flag: set to != 0 if this LEB is a copy
 *
 * Allocate an aleb object and initialize the lnum and copy_flag information.
 * The LEB will reference the apeb object, and increment its refcnt.
 */
struct ubi_ainf_leb *ubi_alloc_aleb(struct ubi_attach_info *ai,
				    struct ubi_ainf_peb *apeb,
				    int lnum,
				    int copy_flag)
{
	struct ubi_ainf_leb *aleb;

	ubi_assert(apeb);
	ubi_assert(lnum >= 0);

	aleb = kmem_cache_zalloc(ai->aleb_slab_cache, GFP_KERNEL);
	if (!aleb)
		return NULL;

	aleb->peb = apeb;
	aleb->copy_flag = !!copy_flag;
	aleb->lnum = lnum;

	return aleb;
}

/**
 * ubi_free_aleb - free an aleb element
 * @ai: attaching information
 * @aleb: the element to free
 *
 * Free an aleb object. The caller must have removed the element from the
 * RB tree.
 */
void ubi_free_aleb(struct ubi_attach_info *ai, struct ubi_ainf_leb *aleb,
		   struct list_head *list)
{
	struct ubi_ainf_peb *apeb = aleb->peb;

	if (apeb) {
		if (!apeb->consolidated || !--apeb->mleb.refcnt) {
			if (!list)
				ubi_free_apeb(ai, apeb);
			else
				add_apeb_to_list(ai, apeb, 0, list);
		}
	}

	kmem_cache_free(ai->aleb_slab_cache, aleb);
}

/**
 * add_to_list - add physical eraseblock to a list.
 * @ai: attaching information
 * @pnum: physical eraseblock number to add
 * @vol_id: the last used volume id for the PEB
 * @lnum: the last used LEB number for the PEB
 * @ec: erase counter of the physical eraseblock
 * @to_head: if not zero, add to the head of the list
 * @list: the list to add to
 *
 * This function allocates a 'struct ubi_ainf_peb' object for physical
 * eraseblock @pnum and adds it to the "free", "erase", or "alien" lists.
 * It stores the @lnum and @vol_id alongside, which can both be
 * %UBI_UNKNOWN if they are not available, not readable, or not assigned.
 * If @to_head is not zero, PEB will be added to the head of the list, which
 * basically means it will be processed first later. E.g., we add corrupted
 * PEBs (corrupted due to power cuts) to the head of the erase list to make
 * sure we erase them first and get rid of corruptions ASAP. This function
 * returns zero in case of success and a negative error code in case of
 * failure.
 */
static int add_to_list(struct ubi_attach_info *ai, int pnum, int vol_id,
		       int lnum, int ec, int to_head, struct list_head *list)
{
	struct ubi_ainf_peb *apeb;

	apeb = ubi_alloc_apeb(ai, pnum, ec);
	if (!apeb)
		return -ENOMEM;

	apeb->vol_id = vol_id;
	apeb->sleb.lnum = lnum;

	add_apeb_to_list(ai, apeb, to_head, list);

	return 0;
}

/**
 * add_corrupted - add a corrupted physical eraseblock.
 * @ai: attaching information
 * @pnum: physical eraseblock number to add
 * @ec: erase counter of the physical eraseblock
 *
 * This function allocates a 'struct ubi_ainf_peb' object for a corrupted
 * physical eraseblock @pnum and adds it to the 'corr' list.  The corruption
 * was presumably not caused by a power cut. Returns zero in case of success
 * and a negative error code in case of failure.
 */
static int add_corrupted(struct ubi_attach_info *ai, int pnum, int ec)
{
	struct ubi_ainf_peb *apeb;

	dbg_bld("add to corrupted: PEB %d, EC %d", pnum, ec);

	apeb = ubi_alloc_apeb(ai, pnum, ec);
	if (!apeb)
		return -ENOMEM;

	ai->corr_peb_count += 1;
	list_add(&apeb->node, &ai->corr);
	return 0;
}

/**
 * add_fastmap - add a Fastmap related physical eraseblock.
 * @ai: attaching information
 * @apeb: physical eraseblock descriptor
 *
 * This function adds a 'struct ubi_ainf_peb' object to the 'fastmap' list.
 * Such blocks can be Fastmap super and data blocks from both the most
 * recent Fastmap we're attaching from or from old Fastmaps which will
 * be erased.
 */
static int add_fastmap(struct ubi_attach_info *ai, struct ubi_ainf_peb *apeb)
{
	/* Fastmap blocks should never be consolidated. */
	if (apeb->consolidated)
		return -EINVAL;

	list_add(&apeb->node, &ai->fastmap);

	dbg_bld("add to fastmap list: PEB %d, vol_id %d, sqnum: %llu",
		ubi_ainf_get_pnum(apeb), apeb->vol_id, apeb->sqnum);

	return 0;
}

/**
 * validate_vid_hdr - check volume identifier header.
 * @ubi: UBI device description object
 * @vid_hdr: the volume identifier header to check
 * @av: information about the volume this logical eraseblock belongs to
 * @pnum: physical eraseblock number the VID header came from
 *
 * This function checks that data stored in @vid_hdr is consistent. Returns
 * non-zero if an inconsistency was found and zero if not.
 *
 * Note, UBI does sanity check of everything it reads from the flash media.
 * Most of the checks are done in the I/O sub-system. Here we check that the
 * information in the VID header is consistent to the information in other VID
 * headers of the same volume.
 */
static int validate_vid_hdr(const struct ubi_device *ubi,
			    const struct ubi_vid_hdr *vid_hdr,
			    const struct ubi_ainf_volume *av, int pnum)
{
	int vol_type = vid_hdr->vol_type;
	int vol_id = be32_to_cpu(vid_hdr->vol_id);
	int used_ebs = be32_to_cpu(vid_hdr->used_ebs);
	int data_pad = be32_to_cpu(vid_hdr->data_pad);

	if (av->leb_count != 0) {
		int av_vol_type;

		/*
		 * This is not the first logical eraseblock belonging to this
		 * volume. Ensure that the data in its VID header is consistent
		 * to the data in previous logical eraseblock headers.
		 */

		if (vol_id != av->vol_id) {
			ubi_err(ubi, "inconsistent vol_id");
			goto bad;
		}

		if (av->vol_type == UBI_STATIC_VOLUME)
			av_vol_type = UBI_VID_STATIC;
		else
			av_vol_type = UBI_VID_DYNAMIC;

		if (vol_type != av_vol_type) {
			ubi_err(ubi, "inconsistent vol_type");
			goto bad;
		}

		if (used_ebs != av->used_ebs) {
			ubi_err(ubi, "inconsistent used_ebs");
			goto bad;
		}

		if (data_pad != av->data_pad) {
			ubi_err(ubi, "inconsistent data_pad");
			goto bad;
		}
	}

	return 0;

bad:
	ubi_err(ubi, "inconsistent VID header at PEB %d", pnum);
	ubi_dump_vid_hdr(vid_hdr);
	ubi_dump_av(av);
	return -EINVAL;
}

/**
 * add_volume - add volume to the attaching information.
 * @ai: attaching information
 * @vol_id: ID of the volume to add
 * @pnum: physical eraseblock number
 * @vid_hdr: volume identifier header
 *
 * If the volume corresponding to the @vid_hdr logical eraseblock is already
 * present in the attaching information, this function does nothing. Otherwise
 * it adds corresponding volume to the attaching information. Returns a pointer
 * to the allocated "av" object in case of success and a negative error code in
 * case of failure.
 */
static struct ubi_ainf_volume *add_volume(struct ubi_attach_info *ai,
					  int vol_id, int pnum,
					  const struct ubi_vid_hdr *vid_hdr)
{
	struct ubi_ainf_volume *av;
	bool created;

	ubi_assert(vol_id == be32_to_cpu(vid_hdr->vol_id));

	av = ubi_find_or_add_av(ai, vol_id, &created);
	if (IS_ERR(av) || !created)
		return av;

	av->used_ebs = be32_to_cpu(vid_hdr->used_ebs);
	av->data_pad = be32_to_cpu(vid_hdr->data_pad);
	av->compat = vid_hdr->compat;
	av->vol_type = vid_hdr->vol_type == UBI_VID_DYNAMIC ? UBI_DYNAMIC_VOLUME
							    : UBI_STATIC_VOLUME;

	return av;
}

/**
 * ubi_compare_lebs - find out which logical eraseblock is newer.
 * @ubi: UBI device description object
 * @aleb: first logical eraseblock to compare
 * @pnum: physical eraseblock number of the second logical eraseblock to
 * compare
 * @vid_hdr: volume identifier header of the second logical eraseblock
 *
 * This function compares 2 copies of a LEB and informs which one is newer. In
 * case of success this function returns a positive value, in case of failure, a
 * negative error code is returned. The success return codes use the following
 * bits:
 *     o bit 0 is cleared: the first PEB (described by @aleb) is newer than the
 *       second PEB (described by @pnum and @vid_hdr);
 *     o bit 0 is set: the second PEB is newer;
 *     o bit 1 is cleared: no bit-flips were detected in the newer LEB;
 *     o bit 1 is set: bit-flips were detected in the newer LEB;
 *     o bit 2 is cleared: the older LEB is not corrupted;
 *     o bit 2 is set: the older LEB is corrupted.
 */
int ubi_compare_lebs(struct ubi_device *ubi, const struct ubi_ainf_leb *aleb,
		     int pnum, const struct ubi_vid_hdr *vid_hdr)
{
	int len, err, second_is_newer, bitflips = 0, corrupted = 0;
	uint32_t data_crc, crc;
	struct ubi_vid_io_buf *vidb = NULL;
	unsigned long long sqnum2 = be64_to_cpu(vid_hdr->sqnum);
	unsigned long long sqnum = ubi_ainf_leb_sqnum(aleb);
	enum ubi_io_mode io_mode;

	if (sqnum2 == sqnum) {
		/*
		 * This must be a really ancient UBI image which has been
		 * created before sequence numbers support has been added. At
		 * that times we used 32-bit LEB versions stored in logical
		 * eraseblocks. That was before UBI got into mainline. We do not
		 * support these images anymore. Well, those images still work,
		 * but only if no unclean reboots happened.
		 */
		ubi_err(ubi, "unsupported on-flash UBI format");
		return -EINVAL;
	}

	/* Obviously the LEB with lower sequence counter is older */
	second_is_newer = (sqnum2 > sqnum);

	/*
	 * Now we know which copy is newer. If the copy flag of the PEB with
	 * newer version is not set, then we just return, otherwise we have to
	 * check data CRC. For the second PEB we already have the VID header,
	 * for the first one - we'll need to re-read it from flash.
	 *
	 * Note: this may be optimized so that we wouldn't read twice.
	 */

	if (second_is_newer) {
		if (!vid_hdr->copy_flag) {
			/* It is not a copy, so it is newer */
			dbg_bld("second PEB %d is newer, copy_flag is unset",
				pnum);
			return 1;
		}
	} else {
		if (!aleb->copy_flag) {
			/* It is not a copy, so it is newer */
			dbg_bld("first PEB %d is newer, copy_flag is unset",
				pnum);
			return bitflips << 1;
		}

		vidb = ubi_alloc_vid_buf(ubi, GFP_KERNEL);
		if (!vidb)
			return -ENOMEM;

		pnum = ubi_ainf_get_pnum(aleb->peb);
		err = ubi_io_read_vid_hdr(ubi, pnum, vidb, 0);
		if (err) {
			if (err == UBI_IO_BITFLIPS)
				bitflips = 1;
			else {
				ubi_err(ubi, "VID of PEB %d header is bad, but it was OK earlier, err %d",
					pnum, err);
				if (err > 0)
					err = -EIO;

				goto out_free_vidh;
			}
		}

		vid_hdr = ubi_get_vid_hdr(vidb);
	}

	/* Read the data of the copy and check the CRC */

	len = be32_to_cpu(vid_hdr->data_size);

	io_mode = ubi_io_mode_from_vid_hdr(vid_hdr);

	mutex_lock(&ubi->buf_mutex);
	err = ubi_io_read_data(ubi, ubi->peb_buf, pnum, 0, len, io_mode);
	if (err && err != UBI_IO_BITFLIPS && !mtd_is_eccerr(err))
		goto out_unlock;

	data_crc = be32_to_cpu(vid_hdr->data_crc);
	crc = crc32(UBI_CRC32_INIT, ubi->peb_buf, len);
	if (crc != data_crc) {
		dbg_bld("PEB %d CRC error: calculated %#08x, must be %#08x",
			pnum, crc, data_crc);
		corrupted = 1;
		bitflips = 0;
		second_is_newer = !second_is_newer;
	} else {
		dbg_bld("PEB %d CRC is OK", pnum);
		bitflips |= !!err;
	}
	mutex_unlock(&ubi->buf_mutex);

	ubi_free_vid_buf(vidb);

	if (second_is_newer)
		dbg_bld("second PEB %d is newer, copy_flag is set", pnum);
	else
		dbg_bld("first PEB %d is newer, copy_flag is set", pnum);

	return second_is_newer | (bitflips << 1) | (corrupted << 2);

out_unlock:
	mutex_unlock(&ubi->buf_mutex);
out_free_vidh:
	ubi_free_vid_buf(vidb);
	return err;
}

/**
 * ubi_add_to_av - add used physical eraseblock to the attaching information.
 * @ubi: UBI device description object
 * @ai: attaching information
 * @apeb: the PEB containing the LEB described by vid_hdr
 * @vid_hdr: the volume identifier header
 *
 * This function adds information about a used physical eraseblock to the
 * 'used' tree of the corresponding volume. The function is rather complex
 * because it has to handle cases when this is not the first physical
 * eraseblock belonging to the same logical eraseblock, and the newer one has
 * to be picked, while the older one has to be dropped. This function returns
 * zero in case of success and a negative error code in case of failure.
 */
int ubi_add_to_av(struct ubi_device *ubi, struct ubi_attach_info *ai,
		  struct ubi_ainf_peb *apeb,
		  const struct ubi_vid_hdr *vid_hdr)
{
	int err, vol_id, vol_mode, lnum, pnum, lpos;
	unsigned long long sqnum;
	struct ubi_ainf_volume *av;
	struct ubi_ainf_leb *aleb;
	struct rb_node **p, *parent = NULL;

	vol_id = be32_to_cpu(vid_hdr->vol_id);
	vol_mode = ubi_vol_mode_from_vid_hdr(vid_hdr);
	lnum = be32_to_cpu(vid_hdr->lnum);
	sqnum = be64_to_cpu(vid_hdr->sqnum);
	pnum = ubi_ainf_get_pnum(apeb);
	lpos = vid_hdr->lpos;

	ubi_assert(sqnum == apeb->sqnum + lpos);

	dbg_bld("PEB %d, LEB %d:%d, EC %d, sqnum %llu, bitflips %d",
		pnum, vol_id, lnum, apeb->ec, sqnum, apeb->scrub);

	av = add_volume(ai, vol_id, pnum, vid_hdr);
	if (IS_ERR(av))
		return PTR_ERR(av);

	/* Assign the volume mode if it's just been created. */
	if (av->vol_mode < 0)
		av->vol_mode = vol_mode;

	/* All VID headers in a given volume should expose the same mode. */
	if (vol_mode != av->vol_mode) {
		ubi_err(ubi, "invalid mode detected: got %d expected %d",
			vid_hdr->vol_mode, av->vol_mode);
		return -EINVAL;
	}

	if (ai->max_sqnum < sqnum)
		ai->max_sqnum = sqnum;

	/*
	 * Walk the RB-tree of logical eraseblocks of volume @vol_id to look
	 * if this is the first instance of this logical eraseblock or not.
	 */
	p = &av->root.rb_node;
	while (*p) {
		int cmp_res;

		parent = *p;
		aleb = rb_entry(parent, struct ubi_ainf_leb, node);
		if (lnum != aleb->lnum) {
			if (lnum < aleb->lnum)
				p = &(*p)->rb_left;
			else
				p = &(*p)->rb_right;
			continue;
		}

		/*
		 * There is already a physical eraseblock describing the same
		 * logical eraseblock present.
		 */

		dbg_bld("this LEB already exists: PEB %d, sqnum %llu, EC %d",
			ubi_ainf_get_pnum(aleb->peb), ubi_ainf_leb_sqnum(aleb),
			aleb->peb->ec);

		/*
		 * Make sure that the logical eraseblocks have different
		 * sequence numbers. Otherwise the image is bad.
		 *
		 * However, if the sequence number is zero, we assume it must
		 * be an ancient UBI image from the era when UBI did not have
		 * sequence numbers. We still can attach these images, unless
		 * there is a need to distinguish between old and new
		 * eraseblocks, in which case we'll refuse the image in
		 * 'ubi_compare_lebs()'. In other words, we attach old clean
		 * images, but refuse attaching old images with duplicated
		 * logical eraseblocks because there was an unclean reboot.
		 */
		if (ubi_ainf_leb_sqnum(aleb) == sqnum && sqnum != 0) {
			ubi_err(ubi, "two LEBs with same sequence number %llu",
				sqnum);
			ubi_dump_aleb(aleb, 0);
			ubi_dump_vid_hdr(vid_hdr);
			return -EINVAL;
		}

		/*
		 * Now we have to drop the older one and preserve the newer
		 * one.
		 */
		cmp_res = ubi_compare_lebs(ubi, aleb, pnum, vid_hdr);
		if (cmp_res < 0)
			return cmp_res;

		if (cmp_res & 1) {
			/*
			 * This logical eraseblock is newer than the one
			 * found earlier.
			 */
			err = validate_vid_hdr(ubi, vid_hdr, av, pnum);
			if (err)
				return err;

			if (!ubi_ainf_dec_apeb_refcnt(aleb->peb)) {
				list_del(&aleb->peb->node);
				add_apeb_to_list(ai, aleb->peb, cmp_res & 4,
						 &ai->erase);
			}

			if (list_empty(&apeb->node))
				list_add_tail(&apeb->node, &ai->used);

			if ((cmp_res & 2))
				apeb->scrub = true;

			aleb->peb = apeb;
			aleb->copy_flag = vid_hdr->copy_flag;

			if (av->highest_lnum == lnum)
				av->last_data_size =
					be32_to_cpu(vid_hdr->data_size);

			return 0;
		} else {
			/*
			 * This logical eraseblock is older than the one found
			 * previously.
			 */
			if (!ubi_ainf_dec_apeb_refcnt(apeb))
				add_apeb_to_list(ai, apeb, cmp_res & 4,
						 &ai->erase);

			return 0;
		}
	}

	/*
	 * We've met this logical eraseblock for the first time, add it to the
	 * attaching information.
	 */

	err = validate_vid_hdr(ubi, vid_hdr, av, pnum);
	if (err)
		return err;

	aleb = ubi_alloc_aleb(ai, apeb, lnum, vid_hdr->copy_flag);
	if (!aleb)
		return -ENOMEM;

	if (list_empty(&apeb->node))
		list_add_tail(&apeb->node, &ai->used);

	if (av->highest_lnum <= lnum) {
		av->highest_lnum = lnum;
		av->last_data_size = be32_to_cpu(vid_hdr->data_size);
	}

	av->leb_count += 1;
	rb_link_node(&aleb->node, parent, p);
	rb_insert_color(&aleb->node, &av->root);
	return 0;
}

/**
 * ubi_add_av - add volume to the attaching information.
 * @ai: attaching information
 * @vol_id: the requested volume ID
 *
 * This function returns a pointer to the new volume description or an
 * ERR_PTR if the operation failed.
 */
struct ubi_ainf_volume *ubi_add_av(struct ubi_attach_info *ai, int vol_id)
{
	bool created;

	return find_or_add_av(ai, vol_id, AV_ADD, &created);
}

/**
 * ubi_find_av - find volume in the attaching information.
 * @ai: attaching information
 * @vol_id: the requested volume ID
 *
 * This function returns a pointer to the volume description or %NULL if there
 * are no data about this volume in the attaching information.
 */
struct ubi_ainf_volume *ubi_find_av(const struct ubi_attach_info *ai,
				    int vol_id)
{
	bool created;

	return find_or_add_av((struct ubi_attach_info *)ai, vol_id, AV_FIND,
			      &created);
}

static void destroy_av(struct ubi_attach_info *ai, struct ubi_ainf_volume *av,
		       struct list_head *list);

/**
 * ubi_remove_av - delete attaching information about a volume.
 * @ai: attaching information
 * @av: the volume attaching information to delete
 */
void ubi_remove_av(struct ubi_attach_info *ai, struct ubi_ainf_volume *av)
{
	dbg_bld("remove attaching information about volume %d", av->vol_id);

	rb_erase(&av->rb, &ai->volumes);
	destroy_av(ai, av, &ai->erase);
	ai->vols_found -= 1;
}

/**
 * early_erase_peb - erase a physical eraseblock.
 * @ubi: UBI device description object
 * @ai: attaching information
 * @pnum: physical eraseblock number to erase;
 * @ec: erase counter value to write (%UBI_UNKNOWN if it is unknown)
 *
 * This function erases physical eraseblock 'pnum', and writes the erase
 * counter header to it. This function should only be used on UBI device
 * initialization stages, when the EBA sub-system had not been yet initialized.
 * This function returns zero in case of success and a negative error code in
 * case of failure.
 */
static int early_erase_peb(struct ubi_device *ubi,
			   const struct ubi_attach_info *ai, int pnum, int ec)
{
	int err;
	struct ubi_ec_hdr *ec_hdr;

	if ((long long)ec >= UBI_MAX_ERASECOUNTER) {
		/*
		 * Erase counter overflow. Upgrade UBI and use 64-bit
		 * erase counters internally.
		 */
		ubi_err(ubi, "erase counter overflow at PEB %d, EC %d",
			pnum, ec);
		return -EINVAL;
	}

	ec_hdr = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
	if (!ec_hdr)
		return -ENOMEM;

	ec_hdr->ec = cpu_to_be64(ec);

	err = ubi_io_sync_erase(ubi, pnum, 0);
	if (err < 0)
		goto out_free;

	err = ubi_io_write_ec_hdr(ubi, pnum, ec_hdr);

out_free:
	kfree(ec_hdr);
	return err;
}

/**
 * ubi_early_get_peb - get a free physical eraseblock.
 * @ubi: UBI device description object
 * @ai: attaching information
 *
 * This function returns a free physical eraseblock. It is supposed to be
 * called on the UBI initialization stages when the wear-leveling sub-system is
 * not initialized yet. This function picks a physical eraseblocks from one of
 * the lists, writes the EC header if it is needed, and removes it from the
 * list.
 *
 * This function returns a pointer to the "apeb" of the found free PEB in case
 * of success and an error code in case of failure.
 */
struct ubi_ainf_peb *ubi_early_get_peb(struct ubi_device *ubi,
				       struct ubi_attach_info *ai)
{
	int err = 0;
	struct ubi_ainf_peb *apeb, *tmp_apeb;

	if (!list_empty(&ai->free)) {
		apeb = list_entry(ai->free.next, struct ubi_ainf_peb, node);
		list_del_init(&apeb->node);
		dbg_bld("return free PEB %d, EC %d", apeb->sleb.pnum, apeb->ec);
		return apeb;
	}

	/*
	 * We try to erase the first physical eraseblock from the erase list
	 * and pick it if we succeed, or try to erase the next one if not. And
	 * so forth. We don't want to take care about bad eraseblocks here -
	 * they'll be handled later.
	 */
	list_for_each_entry_safe(apeb, tmp_apeb, &ai->erase, node) {
		int pnum = ubi_ainf_get_pnum(apeb);

		if (apeb->ec == UBI_UNKNOWN)
			apeb->ec = ai->mean_ec;

		err = early_erase_peb(ubi, ai, pnum, apeb->ec + 1);
		if (err)
			continue;

		/* Free the cpeb object if the PEB was consolidated. */
		if (apeb->consolidated) {
			kfree(apeb->mleb.cpeb);
			apeb->consolidated = 0;
		}

		apeb->ec += 1;
		apeb->sleb.pnum = pnum;
		apeb->sleb.lnum = UBI_UNKNOWN;

		list_del_init(&apeb->node);
		dbg_bld("return PEB %d, EC %d", pnum, apeb->ec);
		return apeb;
	}

	ubi_err(ubi, "no free eraseblocks");
	return ERR_PTR(-ENOSPC);
}

/**
 * check_corruption - check the data area of PEB.
 * @ubi: UBI device description object
 * @vid_hdr: the (corrupted) VID header of this PEB
 * @pnum: the physical eraseblock number to check
 *
 * This is a helper function which is used to distinguish between VID header
 * corruptions caused by power cuts and other reasons. If the PEB contains only
 * 0xFF bytes in the data area, the VID header is most probably corrupted
 * because of a power cut (%0 is returned in this case). Otherwise, it was
 * probably corrupted for some other reasons (%1 is returned in this case). A
 * negative error code is returned if a read error occurred.
 *
 * If the corruption reason was a power cut, UBI can safely erase this PEB.
 * Otherwise, it should preserve it to avoid possibly destroying important
 * information.
 */
static int check_corruption(struct ubi_device *ubi, struct ubi_vid_hdr *vid_hdr,
			    int pnum)
{
	int err;

	mutex_lock(&ubi->buf_mutex);
	memset(ubi->peb_buf, 0x00, ubi->leb_size);

	err = ubi_io_read(ubi, ubi->peb_buf, pnum, ubi->leb_start,
			  ubi->leb_size, UBI_IO_MODE_NORMAL);
	if (err == UBI_IO_BITFLIPS || mtd_is_eccerr(err)) {
		/*
		 * Bit-flips or integrity errors while reading the data area.
		 * It is difficult to say for sure what type of corruption is
		 * this, but presumably a power cut happened while this PEB was
		 * erased, so it became unstable and corrupted, and should be
		 * erased.
		 */
		err = 0;
		goto out_unlock;
	}

	if (err)
		goto out_unlock;

	if (ubi_check_pattern(ubi->peb_buf, 0xFF, ubi->leb_size))
		goto out_unlock;

	ubi_err(ubi, "PEB %d contains corrupted VID header, and the data does not contain all 0xFF",
		pnum);
	ubi_err(ubi, "this may be a non-UBI PEB or a severe VID header corruption which requires manual inspection");
	ubi_dump_vid_hdr(vid_hdr);
	pr_err("hexdump of PEB %d offset %d, length %d",
	       pnum, ubi->leb_start, ubi->leb_size);
	ubi_dbg_print_hex_dump(KERN_DEBUG, "", DUMP_PREFIX_OFFSET, 32, 1,
			       ubi->peb_buf, ubi->leb_size, 1);
	err = 1;

out_unlock:
	mutex_unlock(&ubi->buf_mutex);
	return err;
}

static bool vol_ignored(int vol_id)
{
	switch (vol_id) {
		case UBI_LAYOUT_VOLUME_ID:
		return true;
	}

#ifdef CONFIG_MTD_UBI_FASTMAP
	return ubi_is_fm_vol(vol_id);
#else
	return false;
#endif
}

static struct ubi_ainf_peb *vidb_to_apeb(struct ubi_device *ubi,
					 struct ubi_attach_info *ai,
					 int pnum, int ec,
					 struct ubi_vid_io_buf *vidb,
					 int bitflips)
{
	struct ubi_vid_hdr *vidh = ubi_get_vid_hdr(vidb);
	int i, nhdrs = ubi_get_nhdrs(vidb);
	struct ubi_consolidated_peb *cpeb;
	struct ubi_ainf_peb *apeb;

	apeb = ubi_alloc_apeb(ai, pnum, ec);
	if (!apeb)
		return ERR_PTR(-ENOMEM);

	ubi_assert(nhdrs == 1 || nhdrs == ubi->max_lebs_per_peb);

	apeb->scrub = !!bitflips;
	apeb->vol_id = be32_to_cpu(vidh->vol_id);
	apeb->sqnum = be64_to_cpu(vidh->sqnum);

	if (ubi_get_nhdrs(vidb) == 1) {
		apeb->sleb.lnum = be32_to_cpu(vidh->lnum);
		return apeb;
	}

	cpeb = kmalloc(sizeof(*cpeb) + (nhdrs * sizeof(*cpeb->lnums)),
		       GFP_KERNEL);
	if (!cpeb) {
		ubi_free_apeb(ai, apeb);
		return ERR_PTR(-ENOMEM);
	}

	apeb->consolidated = 1;
	apeb->mleb.cpeb = cpeb;
	apeb->mleb.refcnt = 0;

	cpeb->pnum = pnum;
	for (i = 0; i < nhdrs; i++) {
		cpeb->lnums[i] = be32_to_cpu(vidh[i].lnum);

		if (vidh[i].lpos != UBI_VID_LPOS_INVALID)
			apeb->mleb.refcnt++;
	}

	return apeb;
}

/**
 * scan_peb - scan and process UBI headers of a PEB.
 * @ubi: UBI device description object
 * @ai: attaching information
 * @pnum: the physical eraseblock number
 * @fast: true if we're scanning for a Fastmap
 *
 * This function reads UBI headers of PEB @pnum, checks them, and adds
 * information about this PEB to the corresponding list or RB-tree in the
 * "attaching info" structure. Returns zero if the physical eraseblock was
 * successfully handled and a negative error code in case of failure.
 */
static int scan_peb(struct ubi_device *ubi, struct ubi_attach_info *ai,
		    int pnum, bool fast)
{
	struct ubi_ec_hdr *ech = ai->ech;
	struct ubi_vid_io_buf *vidb = ai->vidb;
	struct ubi_vid_hdr *vidh = ubi_get_vid_hdr(vidb);
	long long ec;
	int err, bitflips = 0, vol_id = -1, ec_err = 0;
	struct ubi_ainf_peb *apeb;
	int version = -1;

	dbg_bld("scan PEB %d", pnum);

	/* Skip bad physical eraseblocks */
	err = ubi_io_is_bad(ubi, pnum);
	if (err < 0)
		return err;
	else if (err) {
		ai->bad_peb_count += 1;
		return 0;
	}

	err = ubi_io_read_ec_hdr(ubi, pnum, ech, 0);
	if (err < 0)
		return err;
	switch (err) {
	case 0:
		break;
	case UBI_IO_BITFLIPS:
		bitflips = 1;
		break;
	case UBI_IO_FF:
		ai->empty_peb_count += 1;
		return add_to_list(ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
				   UBI_UNKNOWN, 0, &ai->erase);
	case UBI_IO_FF_BITFLIPS:
		ai->empty_peb_count += 1;
		return add_to_list(ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
				   UBI_UNKNOWN, 1, &ai->erase);
	case UBI_IO_BAD_HDR_EBADMSG:
	case UBI_IO_BAD_HDR:
		/*
		 * We have to also look at the VID header, possibly it is not
		 * corrupted. Set %bitflips flag in order to make this PEB be
		 * moved and EC be re-created.
		 */
		ec_err = err;
		ec = UBI_UNKNOWN;
		bitflips = 1;
		break;
	default:
		ubi_err(ubi, "'ubi_io_read_ec_hdr()' returned unknown code %d",
			err);
		return -EINVAL;
	}

	if (!ec_err) {
		int image_seq;

		/* Initialize the version value to the EC header one. */
		version = ech->version;

		ec = be64_to_cpu(ech->ec);
		if (ec > UBI_MAX_ERASECOUNTER) {
			/*
			 * Erase counter overflow. The EC headers have 64 bits
			 * reserved, but we anyway make use of only 31 bit
			 * values, as this seems to be enough for any existing
			 * flash. Upgrade UBI and use 64-bit erase counters
			 * internally.
			 */
			ubi_err(ubi, "erase counter overflow, max is %d",
				UBI_MAX_ERASECOUNTER);
			ubi_dump_ec_hdr(ech);
			return -EINVAL;
		}

		/*
		 * Make sure that all PEBs have the same image sequence number.
		 * This allows us to detect situations when users flash UBI
		 * images incorrectly, so that the flash has the new UBI image
		 * and leftovers from the old one. This feature was added
		 * relatively recently, and the sequence number was always
		 * zero, because old UBI implementations always set it to zero.
		 * For this reasons, we do not panic if some PEBs have zero
		 * sequence number, while other PEBs have non-zero sequence
		 * number.
		 */
		image_seq = be32_to_cpu(ech->image_seq);
		if (!ubi->image_seq)
			ubi->image_seq = image_seq;
		if (image_seq && ubi->image_seq != image_seq) {
			ubi_err(ubi, "bad image sequence number %d in PEB %d, expected %d",
				image_seq, pnum, ubi->image_seq);
			ubi_dump_ec_hdr(ech);
			return -EINVAL;
		}
	}

	/* OK, we've done with the EC header, let's look at the VID header */

	err = ubi_io_read_vid_hdr(ubi, pnum, vidb, 0);
	if (err < 0)
		return err;

	switch (err) {
	case 0:
		break;
	case UBI_IO_BITFLIPS:
		bitflips = 1;
		break;
	case UBI_IO_BAD_HDR_EBADMSG:
		if (ec_err == UBI_IO_BAD_HDR_EBADMSG)
			/*
			 * Both EC and VID headers are corrupted and were read
			 * with data integrity error, probably this is a bad
			 * PEB, bit it is not marked as bad yet. This may also
			 * be a result of power cut during erasure.
			 */
			ai->maybe_bad_peb_count += 1;
	case UBI_IO_BAD_HDR:
			/*
			 * If we're facing a bad VID header we have to drop *all*
			 * Fastmap data structures we find. The most recent Fastmap
			 * could be bad and therefore there is a chance that we attach
			 * from an old one. On a fine MTD stack a PEB must not render
			 * bad all of a sudden, but the reality is different.
			 * So, let's be paranoid and help finding the root cause by
			 * falling back to scanning mode instead of attaching with a
			 * bad EBA table and cause data corruption which is hard to
			 * analyze.
			 */
			if (fast)
				ai->force_full_scan = 1;

		if (ec_err)
			/*
			 * Both headers are corrupted. There is a possibility
			 * that this a valid UBI PEB which has corresponding
			 * LEB, but the headers are corrupted. However, it is
			 * impossible to distinguish it from a PEB which just
			 * contains garbage because of a power cut during erase
			 * operation. So we just schedule this PEB for erasure.
			 *
			 * Besides, in case of NOR flash, we deliberately
			 * corrupt both headers because NOR flash erasure is
			 * slow and can start from the end.
			 */
			err = 0;
		else
			/*
			 * The EC was OK, but the VID header is corrupted. We
			 * have to check what is in the data area.
			 */
			err = check_corruption(ubi, vidh, pnum);

		if (err < 0)
			return err;
		else if (!err)
			/* This corruption is caused by a power cut */
			err = add_to_list(ai, pnum, UBI_UNKNOWN,
					  UBI_UNKNOWN, ec, 1, &ai->erase);
		else
			/* This is an unexpected corruption */
			err = add_corrupted(ai, pnum, ec);
		if (err)
			return err;
		goto adjust_mean_ec;
	case UBI_IO_FF_BITFLIPS:
		err = add_to_list(ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
				  ec, 1, &ai->erase);
		if (err)
			return err;
		goto adjust_mean_ec;
	case UBI_IO_FF:
		if (ec_err || bitflips)
			err = add_to_list(ai, pnum, UBI_UNKNOWN,
					  UBI_UNKNOWN, ec, 1, &ai->erase);
		else
			err = add_to_list(ai, pnum, UBI_UNKNOWN,
					  UBI_UNKNOWN, ec, 0, &ai->free);
		if (err)
			return err;
		goto adjust_mean_ec;
	case UBI_IO_INCOMPLETE_CONSO:
		err = add_to_list(ai, pnum, UBI_UNKNOWN, UBI_UNKNOWN,
				  ec, 1, &ai->erase);
		if (err)
			return err;
		goto adjust_mean_ec;
	default:
		ubi_err(ubi, "'ubi_io_read_vid_hdr()' returned unknown code %d",
			err);
		return -EINVAL;
	}

	apeb = vidb_to_apeb(ubi, ai, pnum, ec, vidb, bitflips);
	if (IS_ERR(apeb))
		return PTR_ERR(apeb);

	/*
	 * version might be < 0 if the EC header is corrupted. In this case,
	 * pick the version found in the VID header.
	 */
	if (version < 0)
		version = vidh->version;

	/* Make sure both VID header and EC header version value match. */
	if (vidh->version != version)
		ubi_err(ubi,
			"version in VID and EC headers do not match (%d %d)",
			(int)vidh->version, (int)version);

	/*
	 * Initialize the UBI device version if it's the first valid PEB we are
	 * scanning.
	 */
	if (ubi->version < 0)
		ubi->version = version;

	vol_id = be32_to_cpu(vidh->vol_id);
	if (vol_id > UBI_MAX_VOLUMES && !vol_ignored(vol_id)) {
		/* Unsupported internal volume */
		switch (vidh->compat) {
		case UBI_COMPAT_DELETE:
			ubi_msg(ubi, "\"delete\" compatible internal volume %d found, will remove it",
				vol_id);

			add_apeb_to_list(ai, apeb, 1, &ai->erase);
			return 0;

		case UBI_COMPAT_RO:
			ubi_msg(ubi, "read-only compatible internal volume %d found, switch to read-only mode",
				vol_id);
			ubi->ro_mode = 1;
			break;

		case UBI_COMPAT_PRESERVE:
			ubi_msg(ubi, "\"preserve\" compatible internal volume %d found",
				vol_id);
			add_apeb_to_list(ai, apeb, 0, &ai->alien);
			return 0;

		case UBI_COMPAT_REJECT:
			ubi_err(ubi, "incompatible internal volume %d found",
				vol_id);
			return -EINVAL;
		}
	}

	if (ec_err)
		ubi_warn(ubi, "valid VID header but corrupted EC header at PEB %d",
			 pnum);

	if (ubi_is_fm_vol(vol_id)) {
		err = add_fastmap(ai, apeb);
		if (err)
			ubi_free_apeb(ai, apeb);
	} else {
		int i;

		/* Try to add all headers. */
		for (i = 0; i < ubi_get_nhdrs(vidb); i++) {
			if (vidh[i].lpos == UBI_VID_LPOS_INVALID)
				continue;

			err = ubi_add_to_av(ubi, ai, apeb, &vidh[i]);
			if (err)
				break;
		}

		/*
		 * i != ubi_get_nhdrs(vidb) means we had an error.
		 * Decrement the ref counter and free the apeb element if it's
		 * no longer referenced.
		 */
		for (; i < ubi_get_nhdrs(vidb); i++) {
			if (vidh[i].lpos == UBI_VID_LPOS_INVALID)
				continue;

			if (!ubi_ainf_dec_apeb_refcnt(apeb)) {
				ubi_free_apeb(ai, apeb);
				break;
			}
		}
	}

	if (err)
		return err;

adjust_mean_ec:
	if (!ec_err) {
		ai->ec_sum += ec;
		ai->ec_count += 1;
		if (ec > ai->max_ec)
			ai->max_ec = ec;
		if (ec < ai->min_ec)
			ai->min_ec = ec;
	}

	return 0;
}

/**
 * late_analysis - analyze the overall situation with PEB.
 * @ubi: UBI device description object
 * @ai: attaching information
 *
 * This is a helper function which takes a look what PEBs we have after we
 * gather information about all of them ("ai" is compete). It decides whether
 * the flash is empty and should be formatted of whether there are too many
 * corrupted PEBs and we should not attach this MTD device. Returns zero if we
 * should proceed with attaching the MTD device, and %-EINVAL if we should not.
 */
static int late_analysis(struct ubi_device *ubi, struct ubi_attach_info *ai)
{
	struct ubi_ainf_peb *apeb;
	int max_corr, peb_count;

	peb_count = ubi->peb_count - ai->bad_peb_count - ai->alien_peb_count;
	max_corr = peb_count / 20 ?: 8;

	/*
	 * Few corrupted PEBs is not a problem and may be just a result of
	 * unclean reboots. However, many of them may indicate some problems
	 * with the flash HW or driver.
	 */
	if (ai->corr_peb_count) {
		ubi_err(ubi, "%d PEBs are corrupted and preserved",
			ai->corr_peb_count);
		pr_err("Corrupted PEBs are:");
		list_for_each_entry(apeb, &ai->corr, node)
			pr_cont(" %d", ubi_ainf_get_pnum(apeb));
		pr_cont("\n");

		/*
		 * If too many PEBs are corrupted, we refuse attaching,
		 * otherwise, only print a warning.
		 */
		if (ai->corr_peb_count >= max_corr) {
			ubi_err(ubi, "too many corrupted PEBs, refusing");
			return -EINVAL;
		}
	}

	if (ai->empty_peb_count + ai->maybe_bad_peb_count == peb_count) {
		/*
		 * All PEBs are empty, or almost all - a couple PEBs look like
		 * they may be bad PEBs which were not marked as bad yet.
		 *
		 * This piece of code basically tries to distinguish between
		 * the following situations:
		 *
		 * 1. Flash is empty, but there are few bad PEBs, which are not
		 *    marked as bad so far, and which were read with error. We
		 *    want to go ahead and format this flash. While formatting,
		 *    the faulty PEBs will probably be marked as bad.
		 *
		 * 2. Flash contains non-UBI data and we do not want to format
		 *    it and destroy possibly important information.
		 */
		if (ai->maybe_bad_peb_count <= 2) {
			ai->is_empty = 1;
			ubi_msg(ubi, "empty MTD device detected");
			get_random_bytes(&ubi->image_seq,
					 sizeof(ubi->image_seq));

			/*
			 * Initialize the version to the last supported
			 * version.
			 */
			if (ubi->version < 0)
				ubi->version = UBI_CURRENT_VERSION;
		} else {
			ubi_err(ubi, "MTD device is not UBI-formatted and possibly contains non-UBI data - refusing it");
			return -EINVAL;
		}

	}

	return 0;
}

/**
 * destroy_av - free volume attaching information.
 * @av: volume attaching information
 * @ai: attaching information
 * @list: put the apeb elements in there if !NULL, otherwise free them
 *
 * This function destroys the volume attaching information.
 */
static void destroy_av(struct ubi_attach_info *ai, struct ubi_ainf_volume *av,
		       struct list_head *list)
{
	struct ubi_ainf_leb *aleb;
	struct rb_node *this = av->root.rb_node;

	while (this) {
		if (this->rb_left)
			this = this->rb_left;
		else if (this->rb_right)
			this = this->rb_right;
		else {
			aleb = rb_entry(this, struct ubi_ainf_leb, node);
			this = rb_parent(this);
			if (this) {
				if (this->rb_left == &aleb->node)
					this->rb_left = NULL;
				else
					this->rb_right = NULL;
			}

			ubi_free_aleb(ai, aleb, list);
		}
	}
	kfree(av);
}

/**
 * destroy_ai - destroy attaching information.
 * @ai: attaching information
 */
static void destroy_ai(struct ubi_attach_info *ai)
{
	struct ubi_ainf_peb *apeb, *apeb_tmp;
	struct ubi_ainf_volume *av;
	struct rb_node *rb;

	list_for_each_entry_safe(apeb, apeb_tmp, &ai->alien, node) {
		list_del(&apeb->node);
		ubi_free_apeb(ai, apeb);
	}
	list_for_each_entry_safe(apeb, apeb_tmp, &ai->erase, node) {
		list_del(&apeb->node);
		ubi_free_apeb(ai, apeb);
	}
	list_for_each_entry_safe(apeb, apeb_tmp, &ai->corr, node) {
		list_del(&apeb->node);
		ubi_free_apeb(ai, apeb);
	}
	list_for_each_entry_safe(apeb, apeb_tmp, &ai->free, node) {
		list_del(&apeb->node);
		ubi_free_apeb(ai, apeb);
	}
	list_for_each_entry_safe(apeb, apeb_tmp, &ai->fastmap, node) {
		list_del(&apeb->node);
		ubi_free_apeb(ai, apeb);
	}

	/* Destroy the volume RB-tree */
	rb = ai->volumes.rb_node;
	while (rb) {
		if (rb->rb_left)
			rb = rb->rb_left;
		else if (rb->rb_right)
			rb = rb->rb_right;
		else {
			av = rb_entry(rb, struct ubi_ainf_volume, rb);

			rb = rb_parent(rb);
			if (rb) {
				if (rb->rb_left == &av->rb)
					rb->rb_left = NULL;
				else
					rb->rb_right = NULL;
			}

			destroy_av(ai, av, NULL);
		}
	}

	kmem_cache_destroy(ai->aleb_slab_cache);
	kmem_cache_destroy(ai->apeb_slab_cache);
	kfree(ai);
}

/**
 * scan_all - scan entire MTD device.
 * @ubi: UBI device description object
 * @ai: attach info object
 * @start: start scanning at this PEB
 *
 * This function does full scanning of an MTD device and returns complete
 * information about it in form of a "struct ubi_attach_info" object. In case
 * of failure, an error code is returned.
 */
static int scan_all(struct ubi_device *ubi, struct ubi_attach_info *ai,
		    int start)
{
	int err, pnum;
	struct rb_node *rb1, *rb2;
	struct ubi_ainf_volume *av;
	struct ubi_ainf_peb *apeb;
	struct ubi_ainf_leb *aleb;

	err = -ENOMEM;

	ai->ech = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
	if (!ai->ech)
		return err;

	ai->vidb = ubi_alloc_vid_buf(ubi, GFP_KERNEL);
	if (!ai->vidb)
		goto out_ech;

	for (pnum = start; pnum < ubi->peb_count; pnum++) {
		cond_resched();

		dbg_gen("process PEB %d", pnum);
		err = scan_peb(ubi, ai, pnum, false);
		if (err < 0)
			goto out_vidh;
	}

	ubi_msg(ubi, "scanning is finished");

	/* Calculate mean erase counter */
	if (ai->ec_count)
		ai->mean_ec = div_u64(ai->ec_sum, ai->ec_count);

	err = late_analysis(ubi, ai);
	if (err)
		goto out_vidh;

	/*
	 * In case of unknown erase counter we use the mean erase counter
	 * value.
	 */
	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb) {
		ubi_rb_for_each_entry(rb2, aleb, &av->root, node) {
			if (aleb->peb->ec == UBI_UNKNOWN)
				aleb->peb->ec = ai->mean_ec;
		}
	}

	list_for_each_entry(apeb, &ai->free, node) {
		if (apeb->ec == UBI_UNKNOWN)
			apeb->ec = ai->mean_ec;
	}

	list_for_each_entry(apeb, &ai->corr, node)
		if (apeb->ec == UBI_UNKNOWN)
			apeb->ec = ai->mean_ec;

	list_for_each_entry(apeb, &ai->erase, node)
		if (apeb->ec == UBI_UNKNOWN)
			apeb->ec = ai->mean_ec;

	err = self_check_ai(ubi, ai);
	if (err)
		goto out_vidh;

	ubi_free_vid_buf(ai->vidb);
	kfree(ai->ech);

	return 0;

out_vidh:
	ubi_free_vid_buf(ai->vidb);
out_ech:
	kfree(ai->ech);
	return err;
}

static struct ubi_attach_info *alloc_ai(void)
{
	struct ubi_attach_info *ai;

	ai = kzalloc(sizeof(struct ubi_attach_info), GFP_KERNEL);
	if (!ai)
		return ai;

	INIT_LIST_HEAD(&ai->corr);
	INIT_LIST_HEAD(&ai->free);
	INIT_LIST_HEAD(&ai->used);
	INIT_LIST_HEAD(&ai->erase);
	INIT_LIST_HEAD(&ai->alien);
	INIT_LIST_HEAD(&ai->fastmap);
	ai->volumes = RB_ROOT;
	ai->apeb_slab_cache = kmem_cache_create("ubi_apeb_slab_cache",
					       sizeof(struct ubi_ainf_peb),
					       0, 0, NULL);
	if (!ai->apeb_slab_cache)
		goto err;

	ai->aleb_slab_cache = kmem_cache_create("ubi_aleb_slab_cache",
					       sizeof(struct ubi_ainf_leb),
					       0, 0, NULL);
	if (!ai->aleb_slab_cache)
		goto err;

	return ai;

err:
	kmem_cache_destroy(ai->aleb_slab_cache);
	kmem_cache_destroy(ai->apeb_slab_cache);
	kfree(ai);

	return NULL;
}

#ifdef CONFIG_MTD_UBI_FASTMAP

/**
 * scan_fast - try to find a fastmap and attach from it.
 * @ubi: UBI device description object
 * @ai: attach info object
 *
 * Returns 0 on success, negative return values indicate an internal
 * error.
 * UBI_NO_FASTMAP denotes that no fastmap was found.
 * UBI_BAD_FASTMAP denotes that the found fastmap was invalid.
 */
static int scan_fast(struct ubi_device *ubi, struct ubi_attach_info **ai)
{
	int err, pnum;
	struct ubi_attach_info *scan_ai;

	err = -ENOMEM;

	scan_ai = alloc_ai();
	if (!scan_ai)
		goto out;

	scan_ai->ech = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
	if (!scan_ai->ech)
		goto out_ai;

	scan_ai->vidb = ubi_alloc_vid_buf(ubi, GFP_KERNEL);
	if (!scan_ai->vidb)
		goto out_ech;

	for (pnum = 0; pnum < UBI_FM_MAX_START; pnum++) {
		cond_resched();

		dbg_gen("process PEB %d", pnum);
		err = scan_peb(ubi, scan_ai, pnum, true);
		if (err < 0)
			goto out_vidh;
	}

	ubi_free_vid_buf(scan_ai->vidb);
	kfree(scan_ai->ech);

	if (scan_ai->force_full_scan)
		err = UBI_NO_FASTMAP;
	else
		err = ubi_scan_fastmap(ubi, *ai, scan_ai);

	if (err) {
		/*
		 * Didn't attach via fastmap, do a full scan but reuse what
		 * we've aready scanned.
		 */
		destroy_ai(*ai);
		*ai = scan_ai;
	} else
		destroy_ai(scan_ai);

	return err;

out_vidh:
	ubi_free_vid_buf(scan_ai->vidb);
out_ech:
	kfree(scan_ai->ech);
out_ai:
	destroy_ai(scan_ai);
out:
	return err;
}

#endif

/**
 * ubi_attach - attach an MTD device.
 * @ubi: UBI device descriptor
 * @force_scan: if set to non-zero attach by scanning
 *
 * This function returns zero in case of success and a negative error code in
 * case of failure.
 */
int ubi_attach(struct ubi_device *ubi, int force_scan)
{
	int err;
	struct ubi_attach_info *ai;

	ai = alloc_ai();
	if (!ai)
		return -ENOMEM;

#ifdef CONFIG_MTD_UBI_FASTMAP
	/* On small flash devices we disable fastmap in any case. */
	if ((int)mtd_div_by_eb(ubi->mtd->size, ubi->mtd) <= UBI_FM_MAX_START) {
		ubi->fm_disabled = 1;
		force_scan = 1;
	}

	if (force_scan)
		err = scan_all(ubi, ai, 0);
	else {
		err = scan_fast(ubi, &ai);
		if (err > 0 || mtd_is_eccerr(err)) {
			if (err != UBI_NO_FASTMAP) {
				destroy_ai(ai);
				ai = alloc_ai();
				if (!ai)
					return -ENOMEM;

				err = scan_all(ubi, ai, 0);
			} else {
				err = scan_all(ubi, ai, UBI_FM_MAX_START);
			}
		}
	}
#else
	err = scan_all(ubi, ai, 0);
#endif
	if (err)
		goto out_ai;

	ubi->bad_peb_count = ai->bad_peb_count;
	ubi->good_peb_count = ubi->peb_count - ubi->bad_peb_count;
	ubi->corr_peb_count = ai->corr_peb_count;
	ubi->max_ec = ai->max_ec;
	ubi->mean_ec = ai->mean_ec;
	dbg_gen("max. sequence number:       %llu", ai->max_sqnum);

	err = ubi_read_volume_table(ubi, ai);
	if (err)
		goto out_ai;

	err = ubi_wl_init(ubi, ai);
	if (err)
		goto out_vtbl;

	err = ubi_eba_init(ubi, ai);
	if (err)
		goto out_wl;

#ifdef CONFIG_MTD_UBI_FASTMAP
	if (ubi->fm && ubi_dbg_chk_fastmap(ubi)) {
		struct ubi_attach_info *scan_ai;

		scan_ai = alloc_ai();
		if (!scan_ai) {
			err = -ENOMEM;
			goto out_wl;
		}

		err = scan_all(ubi, scan_ai, 0);
		if (err) {
			destroy_ai(scan_ai);
			goto out_wl;
		}

		err = self_check_eba(ubi, ai, scan_ai);
		destroy_ai(scan_ai);

		if (err)
			goto out_wl;
	}
#endif

	destroy_ai(ai);
	return 0;

out_wl:
	ubi_wl_close(ubi);
out_vtbl:
	ubi_free_internal_volumes(ubi);
	vfree(ubi->vtbl);
out_ai:
	destroy_ai(ai);
	return err;
}

/**
 * self_check_ai - check the attaching information.
 * @ubi: UBI device description object
 * @ai: attaching information
 *
 * This function returns zero if the attaching information is all right, and a
 * negative error code if not or if an error occurred.
 */
static int self_check_ai(struct ubi_device *ubi, struct ubi_attach_info *ai)
{
	struct ubi_vid_io_buf *vidb = ai->vidb;
	struct ubi_vid_hdr *vidh = ubi_get_vid_hdr(vidb);
	int pnum, err, vols_found = 0;
	struct rb_node *rb1, *rb2;
	struct ubi_ainf_volume *av;
	struct ubi_ainf_leb *aleb, *last_aleb;
	struct ubi_ainf_peb *apeb;
	uint8_t *buf;

	if (!ubi_dbg_chk_gen(ubi))
		return 0;

	/*
	 * At first, check that attaching information is OK.
	 */
	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb) {
		int leb_count = 0;

		cond_resched();

		vols_found += 1;

		if (ai->is_empty) {
			ubi_err(ubi, "bad is_empty flag");
			goto bad_av;
		}

		if (av->vol_id < 0 || av->highest_lnum < 0 ||
		    av->leb_count < 0 || av->vol_type < 0 || av->used_ebs < 0 ||
		    av->data_pad < 0 || av->last_data_size < 0) {
			ubi_err(ubi, "negative values");
			goto bad_av;
		}

		if (av->vol_id >= UBI_MAX_VOLUMES &&
		    av->vol_id < UBI_INTERNAL_VOL_START) {
			ubi_err(ubi, "bad vol_id");
			goto bad_av;
		}

		if (av->vol_id > ai->highest_vol_id) {
			ubi_err(ubi, "highest_vol_id is %d, but vol_id %d is there",
				ai->highest_vol_id, av->vol_id);
			goto out;
		}

		if (av->vol_type != UBI_DYNAMIC_VOLUME &&
		    av->vol_type != UBI_STATIC_VOLUME) {
			ubi_err(ubi, "bad vol_type");
			goto bad_av;
		}

		if (av->data_pad > ubi->leb_size / 2) {
			ubi_err(ubi, "bad data_pad");
			goto bad_av;
		}

		last_aleb = NULL;
		ubi_rb_for_each_entry(rb2, aleb, &av->root, node) {
			cond_resched();

			pnum = ubi_ainf_get_pnum(aleb->peb);
			last_aleb = aleb;
			leb_count += 1;

			if (pnum < 0 || aleb->peb->ec < 0) {
				ubi_err(ubi, "negative values");
				goto bad_aleb;
			}

			if (aleb->peb->ec < ai->min_ec) {
				ubi_err(ubi, "bad ai->min_ec (%d), %d found",
					ai->min_ec, aleb->peb->ec);
				goto bad_aleb;
			}

			if (aleb->peb->ec > ai->max_ec) {
				ubi_err(ubi, "bad ai->max_ec (%d), %d found",
					ai->max_ec, aleb->peb->ec);
				goto bad_aleb;
			}

			if (pnum >= ubi->peb_count) {
				ubi_err(ubi, "too high PEB number %d, total PEBs %d",
					pnum, ubi->peb_count);
				goto bad_aleb;
			}

			if (av->vol_type == UBI_STATIC_VOLUME) {
				if (aleb->lnum >= av->used_ebs) {
					ubi_err(ubi, "bad lnum or used_ebs");
					goto bad_aleb;
				}
			} else {
				if (av->used_ebs != 0) {
					ubi_err(ubi, "non-zero used_ebs");
					goto bad_aleb;
				}
			}

			if (aleb->lnum > av->highest_lnum) {
				ubi_err(ubi, "incorrect highest_lnum or lnum");
				goto bad_aleb;
			}
		}

		if (av->leb_count != leb_count) {
			ubi_err(ubi, "bad leb_count, %d objects in the tree",
				leb_count);
			goto bad_av;
		}

		if (!last_aleb)
			continue;

		aleb = last_aleb;

		if (aleb->lnum != av->highest_lnum) {
			ubi_err(ubi, "bad highest_lnum");
			goto bad_aleb;
		}
	}

	if (vols_found != ai->vols_found) {
		ubi_err(ubi, "bad ai->vols_found %d, should be %d",
			ai->vols_found, vols_found);
		goto out;
	}

	/* Check that attaching information is correct */
	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb) {
		last_aleb = NULL;
		ubi_rb_for_each_entry(rb2, aleb, &av->root, node) {
			int vol_type;

			cond_resched();

			last_aleb = aleb;

			err = ubi_io_read_vid_hdr(ubi,
						  ubi_ainf_get_pnum(aleb->peb),
						  vidb, 1);
			if (err && err != UBI_IO_BITFLIPS) {
				ubi_err(ubi, "VID header is not OK (%d)",
					err);
				if (err > 0)
					err = -EIO;
				return err;
			}

			vol_type = vidh->vol_type == UBI_VID_DYNAMIC ?
				   UBI_DYNAMIC_VOLUME : UBI_STATIC_VOLUME;
			if (av->vol_type != vol_type) {
				ubi_err(ubi, "bad vol_type");
				goto bad_vid_hdr;
			}

			if (ubi_ainf_leb_sqnum(aleb) != be64_to_cpu(vidh->sqnum)) {
				ubi_err(ubi, "bad sqnum %llu",
					ubi_ainf_leb_sqnum(aleb));
				goto bad_vid_hdr;
			}

			if (av->vol_id != be32_to_cpu(vidh->vol_id)) {
				ubi_err(ubi, "bad vol_id %d", av->vol_id);
				goto bad_vid_hdr;
			}

			if (av->compat != vidh->compat) {
				ubi_err(ubi, "bad compat %d", vidh->compat);
				goto bad_vid_hdr;
			}

			if (aleb->lnum != be32_to_cpu(vidh->lnum)) {
				ubi_err(ubi, "bad lnum %d", aleb->lnum);
				goto bad_vid_hdr;
			}

			if (av->used_ebs != be32_to_cpu(vidh->used_ebs)) {
				ubi_err(ubi, "bad used_ebs %d", av->used_ebs);
				goto bad_vid_hdr;
			}

			if (av->data_pad != be32_to_cpu(vidh->data_pad)) {
				ubi_err(ubi, "bad data_pad %d", av->data_pad);
				goto bad_vid_hdr;
			}
		}

		if (!last_aleb)
			continue;

		if (av->highest_lnum != be32_to_cpu(vidh->lnum)) {
			ubi_err(ubi, "bad highest_lnum %d", av->highest_lnum);
			goto bad_vid_hdr;
		}

		if (av->last_data_size != be32_to_cpu(vidh->data_size)) {
			ubi_err(ubi, "bad last_data_size %d",
				av->last_data_size);
			goto bad_vid_hdr;
		}
	}

	/*
	 * Make sure that all the physical eraseblocks are in one of the lists
	 * or trees.
	 */
	buf = kzalloc(ubi->peb_count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (pnum = 0; pnum < ubi->peb_count; pnum++) {
		err = ubi_io_is_bad(ubi, pnum);
		if (err < 0) {
			kfree(buf);
			return err;
		} else if (err)
			buf[pnum] = 1;
	}

	ubi_rb_for_each_entry(rb1, av, &ai->volumes, rb)
		ubi_rb_for_each_entry(rb2, aleb, &av->root, node)
			buf[ubi_ainf_get_pnum(aleb->peb)] = 1;

	list_for_each_entry(apeb, &ai->free, node)
		buf[ubi_ainf_get_pnum(aleb->peb)] = 1;

	list_for_each_entry(apeb, &ai->corr, node)
		buf[ubi_ainf_get_pnum(aleb->peb)] = 1;

	list_for_each_entry(apeb, &ai->erase, node)
		buf[ubi_ainf_get_pnum(aleb->peb)] = 1;

	list_for_each_entry(apeb, &ai->alien, node)
		buf[ubi_ainf_get_pnum(aleb->peb)] = 1;

	err = 0;
	for (pnum = 0; pnum < ubi->peb_count; pnum++)
		if (!buf[pnum]) {
			ubi_err(ubi, "PEB %d is not referred", pnum);
			err = 1;
		}

	kfree(buf);
	if (err)
		goto out;
	return 0;

bad_aleb:
	ubi_err(ubi, "bad attaching information about LEB %d", aleb->lnum);
	ubi_dump_aleb(aleb, 0);
	ubi_dump_av(av);
	goto out;

bad_av:
	ubi_err(ubi, "bad attaching information about volume %d", av->vol_id);
	ubi_dump_av(av);
	goto out;

bad_vid_hdr:
	ubi_err(ubi, "bad attaching information about volume %d", av->vol_id);
	ubi_dump_av(av);
	ubi_dump_vid_hdr(vidh);

out:
	dump_stack();
	return -EINVAL;
}
