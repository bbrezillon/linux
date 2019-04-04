/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2014-2018 Broadcom
 * Copyright © 2019 Collabora ltd.
 */
#ifndef _PANFROST_DRM_H_
#define _PANFROST_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_PANFROST_SUBMIT			0x00
#define DRM_PANFROST_WAIT_BO			0x01
#define DRM_PANFROST_CREATE_BO			0x02
#define DRM_PANFROST_MMAP_BO			0x03
#define DRM_PANFROST_GET_PARAM			0x04
#define DRM_PANFROST_GET_BO_OFFSET		0x05
#define DRM_PANFROST_GET_PERFCNT_LAYOUT		0x06
#define DRM_PANFROST_CREATE_PERFMON		0x07
#define DRM_PANFROST_DESTROY_PERFMON		0x08
#define DRM_PANFROST_GET_PERFMON_VALUES		0x09

#define DRM_IOCTL_PANFROST_SUBMIT		DRM_IOW(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT, struct drm_panfrost_submit)
#define DRM_IOCTL_PANFROST_WAIT_BO		DRM_IOW(DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO, struct drm_panfrost_wait_bo)
#define DRM_IOCTL_PANFROST_CREATE_BO		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_CREATE_BO, struct drm_panfrost_create_bo)
#define DRM_IOCTL_PANFROST_MMAP_BO		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_MMAP_BO, struct drm_panfrost_mmap_bo)
#define DRM_IOCTL_PANFROST_GET_PARAM		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_GET_PARAM, struct drm_panfrost_get_param)
#define DRM_IOCTL_PANFROST_GET_BO_OFFSET	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_GET_BO_OFFSET, struct drm_panfrost_get_bo_offset)
#define DRM_IOCTL_PANFROST_GET_PERFCNT_LAYOUT	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_GET_PERFCNT_LAYOUT, struct drm_panfrost_get_perfcnt_layout)
#define DRM_IOCTL_PANFROST_CREATE_PERFMON	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_CREATE_PERFMON, struct drm_panfrost_create_perfmon)
#define DRM_IOCTL_PANFROST_DESTROY_PERFMON	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_DESTROY_PERFMON, struct drm_panfrost_destroy_perfmon)
#define DRM_IOCTL_PANFROST_GET_PERFMON_VALUES	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_GET_PERFMON_VALUES, struct drm_panfrost_get_perfmon_values)

#define PANFROST_JD_REQ_FS (1 << 0)
/**
 * struct drm_panfrost_submit - ioctl argument for submitting commands to the 3D
 * engine.
 *
 * This asks the kernel to have the GPU execute a render command list.
 */
struct drm_panfrost_submit {

	/** Address to GPU mapping of job descriptor */
	__u64 jc;

	/** An optional array of sync objects to wait on before starting this job. */
	__u64 in_syncs;

	/** Number of sync objects to wait on before starting this job. */
	__u32 in_sync_count;

	/** An optional sync object to place the completion fence in. */
	__u32 out_sync;

	/** Pointer to a u32 array of the BOs that are referenced by the job. */
	__u64 bo_handles;

	/** Number of BO handles passed in (size is that times 4). */
	__u32 bo_handle_count;

	/** A combination of PANFROST_JD_REQ_* */
	__u32 requirements;

	/** Pointer to a u32 array of perfmons that should be attached to the job. */
	__u64 perfmon_handles;

	/** Number of perfmon handles passed in (size is that times 4). */
	__u32 perfmon_handle_count;

	/** Unused field, should be set to 0. */
	__u32 padding;
};

/**
 * struct drm_panfrost_wait_bo - ioctl argument for waiting for
 * completion of the last DRM_PANFROST_SUBMIT on a BO.
 *
 * This is useful for cases where multiple processes might be
 * rendering to a BO and you want to wait for all rendering to be
 * completed.
 */
struct drm_panfrost_wait_bo {
	__u32 handle;
	__u32 pad;
	__s64 timeout_ns;	/* absolute */
};

/**
 * struct drm_panfrost_create_bo - ioctl argument for creating Panfrost BOs.
 *
 * There are currently no values for the flags argument, but it may be
 * used in a future extension.
 */
struct drm_panfrost_create_bo {
	__u32 size;
	__u32 flags;
	/** Returned GEM handle for the BO. */
	__u32 handle;
	/* Pad, must be zero-filled. */
	__u32 pad;
	/**
	 * Returned offset for the BO in the GPU address space.  This offset
	 * is private to the DRM fd and is valid for the lifetime of the GEM
	 * handle.
	 *
	 * This offset value will always be nonzero, since various HW
	 * units treat 0 specially.
	 */
	__u64 offset;
};

/**
 * struct drm_panfrost_mmap_bo - ioctl argument for mapping Panfrost BOs.
 *
 * This doesn't actually perform an mmap.  Instead, it returns the
 * offset you need to use in an mmap on the DRM device node.  This
 * means that tools like valgrind end up knowing about the mapped
 * memory.
 *
 * There are currently no values for the flags argument, but it may be
 * used in a future extension.
 */
struct drm_panfrost_mmap_bo {
	/** Handle for the object being mapped. */
	__u32 handle;
	__u32 flags;
	/** offset into the drm node to use for subsequent mmap call. */
	__u64 offset;
};

enum drm_panfrost_param {
	DRM_PANFROST_PARAM_GPU_PROD_ID,
};

struct drm_panfrost_get_param {
	__u32 param;
	__u32 pad;
	__u64 value;
};

/**
 * Returns the offset for the BO in the GPU address space for this DRM fd.
 * This is the same value returned by drm_panfrost_create_bo, if that was called
 * from this DRM fd.
 */
struct drm_panfrost_get_bo_offset {
	__u32 handle;
	__u32 pad;
	__u64 offset;
};

/**
 * Panfrost HW block ids used to group HW counters. There might be several
 * shader, tiler and MMU/L2 blocks in a given GPU. How many of them are
 * available is exposed through the instances field of
 * drm_panfrost_block_perfcounters.
 */
enum drm_panfrost_block_id {
	PANFROST_SHADER_BLOCK,
	PANFROST_TILER_BLOCK,
	PANFROST_MMU_L2_BLOCK,
	PANFROST_JM_BLOCK,
	PANFROST_NUM_BLOCKS,
};

struct drm_panfrost_block_perfcounters {
	/*
	 * For DRM_IOCTL_PANFROST_GET_PERFCNT_LAYOUT, encodes the available
	 * instances for a specific given block type.
	 * For DRM_IOCTL_PANFROST_CREATE_PERFMON, encodes the instances the
	 * user wants to monitor.
	 * Note: the bitmap might be sparse.
	 */
	__u64 instances;

	/*
	 * For DRM_IOCTL_PANFROST_GET_PERFCNT_LAYOUT, encodes the available
	 * counters attached to a specific block type.
	 * For DRM_IOCTL_PANFROST_CREATE_PERFMON, encodes the counters the user
	 * wants to monitor.
	 * Note: the bitmap might be sparse.
	 */
	__u64 counters;
};

/**
 * Used to retrieve available HW counters.
 */
struct drm_panfrost_get_perfcnt_layout {
	struct drm_panfrost_block_perfcounters counters[PANFROST_NUM_BLOCKS];
};

/**
 * Used to create a performance monitor. Each perfmonance monitor is assigned an
 * ID that can later be passed when submitting a job to capture hardware counter
 * values (and thus count things related to this specific job).
 * Performance monitors are attached to the GPU file descriptor and IDs are
 * unique within this context, not across all GPU users.
 * This implies that
 * - perfmons are automatically released when the FD is closed
 * - perfmons can't be shared across GPU context
 */
struct drm_panfrost_create_perfmon {
	/* Input Fields. */
	/* List all HW counters this performance monitor should track. */
	struct drm_panfrost_block_perfcounters counters[PANFROST_NUM_BLOCKS];

	/* Output fields. */
	/* ID of the newly created perfmon. */
	__u32 id;

	/* Padding: must be set to 0. */
	__u32 padding;
};

/**
 * Destroy an existing performance monitor.
 */
struct drm_panfrost_destroy_perfmon {
	/*
	 * ID of the perfmon to destroy (the one returned by
	 * DRM_IOCTL_PANFROST_CREATE_PERFMON)
	 */
	__u32 id;
};

/*
 * Don't wait when trying to get perfmon values. If the perfmon is still active
 * (still attached to a queued or running job), EBUSY is returned.
 */
#define DRM_PANFROST_GET_PERFMON_VALS_DONT_WAIT		0x1

/* Reset all perfmon values to zero after reading them. */
#define DRM_PANFROST_GET_PERFMON_VALS_RESET		0x2

/**
 * Used to query values collected by a performance monitor.
 */
struct drm_panfrost_get_perfmon_values {
	/* ID of the perfmon to query value on. */
	__u32 id;

	/* See DRM_PANFROST_GET_PERFMON_VALS_XXX flags */
	__u32 flags;

	/*
	 * An array of u32 userspace pointers where counters values will be
	 * copied too.
	 * The array sizes depend on the counters/instances activated at
	 * perfmon creation time: hweight64(instances) * hweight64(counters).
	 * Note that some entries in values_ptrs[] might be NULL if no counters
	 * on a specific block were activated.
	 */
	__u64 values_ptrs[PANFROST_NUM_BLOCKS];
};

#if defined(__cplusplus)
}
#endif

#endif /* _PANFROST_DRM_H_ */
