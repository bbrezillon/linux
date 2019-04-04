// SPDX-License-Identifier: GPL-2.0
/* Copyright 2019 Collabora Ltd */

#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/panfrost_drm.h>
#include <linux/iopoll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "panfrost_device.h"
#include "panfrost_features.h"
#include "panfrost_gem.h"
#include "panfrost_issues.h"
#include "panfrost_job.h"
#include "panfrost_mmu.h"
#include "panfrost_regs.h"

#define COUNTERS_PER_BLOCK		64
#define BYTES_PER_COUNTER		4
#define BLOCKS_PER_COREGROUP		8
#define V4_SHADERS_PER_COREGROUP	4

struct panfrost_perfcnt_job_ctx {
	refcount_t refcount;
	struct panfrost_device *pfdev;
	struct dma_fence *wait_fence;
	struct dma_fence *done_fence;
	struct panfrost_perfmon **perfmons;
	u32 perfmon_count;
};

struct panfrost_perfcnt {
	struct work_struct dumpwork;
	u64 fence_context;
	u64 emit_seqno;
	spinlock_t fence_lock;
	struct mutex cfg_lock;
	u32 cur_cfg[PANFROST_NUM_BLOCKS];
	struct panfrost_gem_object *bo;
	void *buf;
	spinlock_t ctx_lock;
	struct panfrost_perfcnt_job_ctx *last_ctx;
	struct panfrost_perfcnt_job_ctx *dump_ctx;
};

struct panfrost_perfcnt_fence {
	struct dma_fence base;
	struct drm_device *dev;
	u64 seqno;
};

struct panfrost_perfmon {
	refcount_t refcnt;
	atomic_t busycnt;
	struct wait_queue_head wq;
	struct drm_panfrost_block_perfcounters counters[PANFROST_NUM_BLOCKS];
	u32 *values[PANFROST_NUM_BLOCKS];
};

static inline struct panfrost_perfcnt_fence *
to_panfrost_perfcnt_fence(struct dma_fence *fence)
{
	return container_of(fence, struct panfrost_perfcnt_fence, base);
}

static const char *
panfrost_perfcnt_fence_get_driver_name(struct dma_fence *fence)
{
	return "panfrost";
}

static const char *
panfrost_perfcnt_fence_get_timeline_name(struct dma_fence *fence)
{
	return "panfrost-perfcnt";
}

static const struct dma_fence_ops panfrost_perfcnt_fence_ops = {
	.get_driver_name = panfrost_perfcnt_fence_get_driver_name,
	.get_timeline_name = panfrost_perfcnt_fence_get_timeline_name,
};

static struct dma_fence *
panfrost_perfcnt_fence_create(struct panfrost_device *pfdev)
{
	struct panfrost_perfcnt_fence *fence;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	fence->dev = pfdev->ddev;
	fence->seqno = ++pfdev->perfcnt->emit_seqno;
	dma_fence_init(&fence->base, &panfrost_perfcnt_fence_ops,
		       &pfdev->perfcnt->fence_lock,
		       pfdev->perfcnt->fence_context, fence->seqno);

	return &fence->base;
}

static void panfrost_perfmon_get(struct panfrost_perfmon *perfmon)
{
	if (perfmon)
		refcount_inc(&perfmon->refcnt);
}

static void panfrost_perfmon_put(struct panfrost_perfmon *perfmon)
{
	if (perfmon && refcount_dec_and_test(&perfmon->refcnt)) {
		unsigned int i;

		for (i = 0; i < PANFROST_NUM_BLOCKS; i++)
			kfree(perfmon->values[i]);

		kfree(perfmon);
	}
}

static struct panfrost_perfmon *
panfrost_perfcnt_find_perfmon(struct panfrost_file_priv *pfile, int id)
{
	struct panfrost_perfmon *perfmon;

	mutex_lock(&pfile->perfmon.lock);
	perfmon = idr_find(&pfile->perfmon.idr, id);
	panfrost_perfmon_get(perfmon);
	mutex_unlock(&pfile->perfmon.lock);

	return perfmon;
}

void panfrost_perfcnt_open(struct panfrost_file_priv *pfile)
{
	mutex_init(&pfile->perfmon.lock);
	idr_init(&pfile->perfmon.idr);
}

static int panfrost_perfcnt_idr_del(int id, void *elem, void *data)
{
	struct panfrost_perfmon *perfmon = elem;

	panfrost_perfmon_put(perfmon);

	return 0;
}

void panfrost_perfcnt_close(struct panfrost_file_priv *pfile)
{
	mutex_lock(&pfile->perfmon.lock);
	idr_for_each(&pfile->perfmon.idr, panfrost_perfcnt_idr_del, NULL);
	idr_destroy(&pfile->perfmon.idr);
	mutex_unlock(&pfile->perfmon.lock);
}

int panfrost_ioctl_get_perfcnt_layout(struct drm_device *dev, void *data,
				      struct drm_file *file_priv)
{
	struct panfrost_file_priv *pfile = file_priv->driver_priv;
	struct panfrost_device *pfdev = pfile->pfdev;
	struct drm_panfrost_get_perfcnt_layout *layout = data;

	memcpy(layout->counters, pfdev->features.perfcnt_layout,
	       sizeof(layout->counters));

	return 0;
}

int panfrost_ioctl_create_perfmon(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	struct panfrost_file_priv *pfile = file_priv->driver_priv;
	struct panfrost_device *pfdev = pfile->pfdev;
	struct drm_panfrost_create_perfmon *req = data;
	struct drm_panfrost_block_perfcounters *layout;
	struct panfrost_perfmon *perfmon;
	unsigned int i;
	int ret;

	if (req->padding)
		return -EINVAL;

	perfmon = kzalloc(sizeof(*perfmon), GFP_KERNEL);
	if (!perfmon)
		return -ENOMEM;

	ret = -ENOMEM;
	layout = pfdev->features.perfcnt_layout;
	for (i = 0; i < PANFROST_NUM_BLOCKS; i++) {
		unsigned int ncounters;

		/* Make sure the request matches the available counters. */
		if (~layout[i].instances & req->counters[i].instances ||
		    ~layout[i].counters & req->counters[i].counters)
			goto err_free_perfmon;

		ncounters = hweight64(req->counters[i].instances) *
			    hweight64(req->counters[i].counters);
		if (!ncounters)
			continue;

		perfmon->counters[i] = req->counters[i];
		perfmon->values[i] = kcalloc(ncounters, sizeof(u32), GFP_KERNEL);
		if (!perfmon->values)
			goto err_free_perfmon;
	}

	refcount_set(&perfmon->refcnt, 1);
	init_waitqueue_head(&perfmon->wq);

	mutex_lock(&pfile->perfmon.lock);
	ret = idr_alloc(&pfile->perfmon.idr, perfmon, 1, U32_MAX, GFP_KERNEL);
	mutex_unlock(&pfile->perfmon.lock);

	if (ret < 0)
		goto err_free_perfmon;

	req->id = ret;
	return 0;

err_free_perfmon:
	for (i = 0; i < PANFROST_NUM_BLOCKS; i++)
		kfree(perfmon->values[i]);

	kfree(perfmon);
	return ret;
}

int panfrost_ioctl_destroy_perfmon(struct drm_device *dev, void *data,
				   struct drm_file *file_priv)
{
	struct panfrost_file_priv *pfile = file_priv->driver_priv;
	struct drm_panfrost_destroy_perfmon *req = data;
	struct panfrost_perfmon *perfmon;

	mutex_lock(&pfile->perfmon.lock);
	perfmon = idr_remove(&pfile->perfmon.idr, req->id);
	mutex_unlock(&pfile->perfmon.lock);

	if (!perfmon)
		return -EINVAL;

	panfrost_perfmon_put(perfmon);
	return 0;
}

int panfrost_ioctl_get_perfmon_values(struct drm_device *dev, void *data,
				      struct drm_file *file_priv)
{
	struct panfrost_file_priv *pfile = file_priv->driver_priv;
	struct drm_panfrost_get_perfmon_values *req = data;
	struct panfrost_perfmon *perfmon;
	unsigned int i;
	int ret = 0;

	mutex_lock(&pfile->perfmon.lock);
	perfmon = idr_find(&pfile->perfmon.idr, req->id);
	panfrost_perfmon_get(perfmon);
	mutex_unlock(&pfile->perfmon.lock);

	if (!perfmon)
		return -EINVAL;

	if (!(req->flags & DRM_PANFROST_GET_PERFMON_VALS_DONT_WAIT))
		ret = wait_event_interruptible(perfmon->wq,
					       !atomic_read(&perfmon->busycnt));
	else if (atomic_read(&perfmon->busycnt))
		ret = -EBUSY;

	if (ret)
		goto out;

	for (i = 0; i < PANFROST_NUM_BLOCKS; i++) {
		unsigned int ncounters;

		ncounters = hweight64(perfmon->counters[i].instances) *
			    hweight64(perfmon->counters[i].counters);
		if (!ncounters)
			continue;

		if (copy_to_user(u64_to_user_ptr(req->values_ptrs[i]),
				 perfmon->values[i],
				 ncounters * sizeof(u32))) {
			ret = -EFAULT;
			break;
		}

		if (req->flags & DRM_PANFROST_GET_PERFMON_VALS_RESET)
			memset(perfmon->values[i], 0, ncounters * sizeof(u32));
	}

out:
	panfrost_perfmon_put(perfmon);
	return ret;
}

/*
 * Returns true if the 2 jobs have exactly the same perfcnt context, false
 * otherwise.
 */
static bool panfrost_perfcnt_job_ctx_cmp(struct panfrost_perfcnt_job_ctx *a,
					 struct panfrost_perfcnt_job_ctx *b)
{
	unsigned int i, j;

	if (a->perfmon_count != b->perfmon_count)
		return false;

	for (i = 0; i < a->perfmon_count; i++) {
		for (j = 0; j < b->perfmon_count; j++) {
			if (a->perfmons[i] == b->perfmons[j])
				break;
		}

		if (j == b->perfmon_count)
			return false;
	}

	return true;
}

static u32 counters_u64_to_u32(u64 in)
{
	unsigned int i;
	u32 out = 0;

	for (i = 0; i < 64; i += 4) {
		if (GENMASK(i + 3, i) & in)
			out |= BIT(i / 4);
	}

	return out;
}

void panfrost_perfcnt_run_job(struct panfrost_job *job)
{
	struct panfrost_perfcnt_job_ctx *ctx = job->perfcnt_ctx;
	struct panfrost_device *pfdev = job->pfdev;
	u32 perfcnt_en[PANFROST_NUM_BLOCKS] = { };
	bool disable_perfcnt = true, config_changed = false;
	unsigned int i, j;
	u64 gpuva;
	u32 cfg;

	mutex_lock(&pfdev->perfcnt->cfg_lock);
	for (i = 0; i < PANFROST_NUM_BLOCKS; i++) {
		for (j = 0; j < ctx->perfmon_count; j++) {
			u64 counters = ctx->perfmons[j]->counters[i].counters;

			perfcnt_en[i] |= counters_u64_to_u32(counters);
		}

		if (perfcnt_en[i])
			disable_perfcnt = false;

		if (perfcnt_en[i] != pfdev->perfcnt->cur_cfg[i]) {
			pfdev->perfcnt->cur_cfg[i] = perfcnt_en[i];
			config_changed = true;
		}
	}
	mutex_unlock(&pfdev->perfcnt->cfg_lock);

	if (!config_changed)
		return;

	/*
	 * Always use address space 0 for now.
	 * FIXME: this needs to be updated when we start using different
	 * address space.
	 */
	cfg = GPU_PERFCNT_CFG_AS(0);
	if (panfrost_model_cmp(pfdev, 0x1000) >= 0)
		cfg |= GPU_PERFCNT_CFG_SETSEL(1);

	gpu_write(pfdev, GPU_PERFCNT_CFG,
		  cfg | GPU_PERFCNT_CFG_MODE(GPU_PERFCNT_CFG_MODE_OFF));

	if (disable_perfcnt)
		return;

	gpu_write(pfdev, GPU_PRFCNT_JM_EN, perfcnt_en[PANFROST_JM_BLOCK]);
	gpu_write(pfdev, GPU_PRFCNT_SHADER_EN,
		  perfcnt_en[PANFROST_SHADER_BLOCK]);
	gpu_write(pfdev, GPU_PRFCNT_MMU_L2_EN,
		  perfcnt_en[PANFROST_MMU_L2_BLOCK]);
	gpuva = pfdev->perfcnt->bo->node.start << PAGE_SHIFT;
	gpu_write(pfdev, GPU_PERFCNT_BASE_LO, gpuva);
	gpu_write(pfdev, GPU_PERFCNT_BASE_HI, gpuva >> 32);

	/*
	 * Due to PRLAM-8186 we need to disable the Tiler before we enable HW
	 * counters.
	 */
	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_8186))
		gpu_write(pfdev, GPU_PRFCNT_TILER_EN, 0);
	else
		gpu_write(pfdev, GPU_PRFCNT_TILER_EN,
			  perfcnt_en[PANFROST_TILER_BLOCK]);

	gpu_write(pfdev, GPU_PERFCNT_CFG,
		  cfg | GPU_PERFCNT_CFG_MODE(GPU_PERFCNT_CFG_MODE_MANUAL));

	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_8186))
		gpu_write(pfdev, GPU_PRFCNT_TILER_EN,
			  perfcnt_en[PANFROST_TILER_BLOCK]);
}

static void
panfrost_perfcnt_release_job_ctx(struct panfrost_perfcnt_job_ctx *ctx)
{
	unsigned int i;

	WARN_ON(refcount_read(&ctx->refcount));
	for (i = 0; i < ctx->perfmon_count; i++) {
		if (atomic_dec_and_test(&ctx->perfmons[i]->busycnt))
			wake_up(&ctx->perfmons[i]->wq);
		panfrost_perfmon_put(ctx->perfmons[i]);
	}

	dma_fence_put(ctx->wait_fence);
	dma_fence_put(ctx->done_fence);
	kfree(ctx->perfmons);
	kfree(ctx);
}

static void panfrost_perfcnt_put_job_ctx(struct panfrost_perfcnt_job_ctx *ctx)
{
	if (!IS_ERR_OR_NULL(ctx) && refcount_dec_and_test(&ctx->refcount))
		panfrost_perfcnt_release_job_ctx(ctx);
}

struct panfrost_perfcnt_job_ctx *
panfrost_perfcnt_get_job_ctx(struct panfrost_perfcnt_job_ctx *ctx)
{
	if (ctx)
		refcount_inc(&ctx->refcount);

	return ctx;
}

static void panfrost_perfcnt_dump_done(struct panfrost_perfcnt_job_ctx *ctx)
{
	struct panfrost_device *pfdev;
	unsigned long flags;

	pfdev = ctx->pfdev;
	spin_lock_irqsave(&pfdev->perfcnt->ctx_lock, flags);
	pfdev->perfcnt->dump_ctx = NULL;
	if (pfdev->perfcnt->last_ctx == ctx)
		pfdev->perfcnt->last_ctx = NULL;
	spin_unlock_irqrestore(&pfdev->perfcnt->ctx_lock, flags);

	dma_fence_signal(ctx->done_fence);
	panfrost_perfcnt_release_job_ctx(ctx);
}

static void
panfrost_perfcnt_get_counter_vals(struct panfrost_device *pfdev,
				  enum drm_panfrost_block_id block,
				  unsigned int instance, u32 *vals)
{
	u64 shader_present = pfdev->features.shader_present;
	unsigned int bufoffs, shaderid, shadernum;

	if (panfrost_has_hw_feature(pfdev, HW_FEATURE_V4)) {
		unsigned int ncoregroups;

		ncoregroups = hweight64(pfdev->features.l2_present);

		switch (block) {
		case PANFROST_SHADER_BLOCK:
			for (shaderid = 0, shadernum = 0; shaderid < 64;
			     shaderid++) {
				if (!(BIT_ULL(shaderid) & shader_present))
					continue;

				if (shadernum == instance)
					break;

				shadernum++;
			}

			if (WARN_ON(shaderid == 64))
				return;

			/* 4 shaders per core group. */
			bufoffs = ((shaderid / V4_SHADERS_PER_COREGROUP) *
				   2048) +
				  ((shaderid % V4_SHADERS_PER_COREGROUP) *
				   256);
			break;

		case PANFROST_TILER_BLOCK:
			if (WARN_ON(instance >= ncoregroups))
				return;

			bufoffs = (instance * 2048) + 1024;
			break;
		case PANFROST_MMU_L2_BLOCK:
			if (WARN_ON(instance >= ncoregroups))
				return;

			bufoffs = (instance * 2048) + 1280;
			break;
		case PANFROST_JM_BLOCK:
			if (WARN_ON(instance))
				return;
			bufoffs = 1792;
			break;
		default:
			WARN_ON(1);
			return;
		}
	} else {
		unsigned int nl2c, ncores;

		/*
		 * TODO: define a macro to extract the number of l2 caches from
		 * mem_features.
		 */
		nl2c = ((pfdev->features.mem_features >> 8) & GENMASK(3, 0)) + 1;

		/*
		 * The ARM driver is grouping cores per core group and then
		 * only using the number of cores in group 0 to calculate the
		 * size. Not sure why this is done like that, but I guess
		 * shader_present will only show cores in the first group
		 * anyway.
		 */
		ncores = hweight64(pfdev->features.shader_present);

		switch (block) {
		case PANFROST_SHADER_BLOCK:
			for (shaderid = 0, shadernum = 0; shaderid < 64;
			     shaderid++) {
				if (!(BIT_ULL(shaderid) & shader_present))
					continue;

				if (shadernum == instance)
					break;

				shadernum++;
			}

			if (WARN_ON(shaderid == 64))
				return;

			/* 4 shaders per core group. */
			bufoffs = 512 + ((nl2c + shaderid) * 256);
			break;

		case PANFROST_TILER_BLOCK:
			if (WARN_ON(instance))
				return;

			bufoffs = 256;
			break;
		case PANFROST_MMU_L2_BLOCK:
			if (WARN_ON(instance >= nl2c))
				return;

			bufoffs = 512 + (instance * 256);
			break;
		case PANFROST_JM_BLOCK:
			if (WARN_ON(instance))
				return;
			bufoffs = 0;
			break;
		default:
			WARN_ON(1);
			return;
		}
	}

	memcpy(vals, pfdev->perfcnt->buf + bufoffs, 256);
}

static void
panfrost_perfmon_upd_counter_vals(struct panfrost_perfmon *perfmon,
				  enum drm_panfrost_block_id block,
				  unsigned int instance, u32 *invals)
{
	u32 *outvals = perfmon->values[block];
	unsigned int inidx, outidx;

	if (WARN_ON(instance >= hweight64(perfmon->counters[block].instances)))
		return;

	if (!(perfmon->counters[block].instances & BIT_ULL(instance)))
		return;

	outvals += instance * hweight64(perfmon->counters[block].counters);
	for (inidx = 0, outidx = 0; inidx < 64; inidx++) {
		if (!(perfmon->counters[block].counters & BIT_ULL(inidx)))
			continue;

		if (U32_MAX - outvals[outidx] < invals[inidx])
			outvals[outidx] = U32_MAX;
		else
			outvals[outidx] += invals[inidx];
		outidx++;
	}
}

static void panfrost_perfcnt_dump_work(struct work_struct *w)
{
	struct panfrost_perfcnt *perfcnt = container_of(w,
						struct panfrost_perfcnt,
						dumpwork);
	struct panfrost_perfcnt_job_ctx *ctx = perfcnt->dump_ctx;
	unsigned int block, instance, pmonidx, num;

	if (!ctx)
		return;

	for (block = 0; block < PANFROST_NUM_BLOCKS; block++) {
		struct panfrost_perfmon *perfmon;
		u32 vals[COUNTERS_PER_BLOCK];
		u64 instances = 0;

		for (pmonidx = 0; pmonidx < ctx->perfmon_count; pmonidx++) {
			perfmon = ctx->perfmons[pmonidx];
			instances |= perfmon->counters[block].instances;
		}

		for (instance = 0, num = 0; instance < 64; instance++) {
			if (!(instances & BIT_ULL(instance)))
				continue;

			panfrost_perfcnt_get_counter_vals(ctx->pfdev, block,
							  instance, vals);

			for (pmonidx = 0; pmonidx < ctx->perfmon_count;
			     pmonidx++) {
				perfmon = ctx->perfmons[pmonidx];
				panfrost_perfmon_upd_counter_vals(perfmon,
								  block,
								  num,
								  vals);
			}
			num++;
		}
	}

	panfrost_perfcnt_dump_done(ctx);
}

void panfrost_perfcnt_clean_cache_done(struct panfrost_device *pfdev)
{
	schedule_work(&pfdev->perfcnt->dumpwork);
}

void panfrost_perfcnt_sample_done(struct panfrost_device *pfdev)
{
	gpu_write(pfdev, GPU_CMD, GPU_CMD_CLEAN_CACHES);
}

void panfrost_perfcnt_clean_job_ctx(struct panfrost_job *job)
{
	return panfrost_perfcnt_put_job_ctx(job->perfcnt_ctx);
}

int panfrost_perfcnt_create_job_ctx(struct panfrost_job *job,
				    struct drm_file *file_priv,
				    struct drm_panfrost_submit *args)
{
	struct panfrost_device *pfdev = job->pfdev;
	struct panfrost_file_priv *pfile = file_priv->driver_priv;
	struct panfrost_perfcnt_job_ctx *ctx;
	unsigned int i, j;
	u32 *handles;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->pfdev = pfdev;
	refcount_set(&ctx->refcount, 1);

	ctx->perfmon_count = args->perfmon_handle_count;
	if (!ctx->perfmon_count) {
		job->perfcnt_ctx = ctx;
		return 0;
	}

	handles = kcalloc(ctx->perfmon_count, sizeof(u32), GFP_KERNEL);
	if (!handles) {
		ret = -ENOMEM;
		goto err_put_ctx;
	}

	if (copy_from_user(handles,
			   u64_to_user_ptr(args->perfmon_handles),
			   ctx->perfmon_count * sizeof(u32))) {
		ret = -EFAULT;
		DRM_DEBUG("Failed to copy in perfmon handles\n");
		goto err_free_handles;
	}

	/* Make sure each perfmon only appears once. */
	for (i = 0; i < ctx->perfmon_count - 1; i++) {
		for (j = i + 1; j < ctx->perfmon_count; j++) {
			if (handles[i] == handles[j]) {
				ret = -EINVAL;
				goto err_free_handles;
			}
		}
	}

	ctx->perfmons = kcalloc(ctx->perfmon_count, sizeof(*ctx->perfmons),
				GFP_KERNEL | __GFP_ZERO);
	if (!ctx->perfmons) {
		ret = -ENOMEM;
		goto err_free_handles;
	}

	for (i = 0; i < ctx->perfmon_count; i++) {
		ctx->perfmons[i] = panfrost_perfcnt_find_perfmon(pfile,
								 handles[i]);
		if (!ctx->perfmons[i]) {
			ret = -EINVAL;
			goto err_free_handles;
		}
		atomic_inc(&ctx->perfmons[i]->busycnt);
	}

	job->perfcnt_ctx = ctx;
	kfree(handles);
	return 0;

err_free_handles:
	kfree(handles);

err_put_ctx:
	panfrost_perfcnt_put_job_ctx(ctx);
	return ret;
}

void panfrost_perfcnt_finish_job(struct panfrost_job *job, bool skip_dump)
{
	struct panfrost_perfcnt_job_ctx *ctx = job->perfcnt_ctx;

	if (WARN_ON(!ctx))
		return;

	job->perfcnt_ctx = NULL;
	if (!refcount_dec_and_test(&ctx->refcount))
		return;

	if (!ctx->perfmon_count || skip_dump) {
		panfrost_perfcnt_dump_done(ctx);
		return;
	}

	ctx->pfdev->perfcnt->dump_ctx = ctx;
	gpu_write(ctx->pfdev, GPU_CMD, GPU_CMD_PERFCNT_SAMPLE);
}

static bool panfrost_perfcnt_try_reuse_last_job_ctx(struct panfrost_job *job)
{
	struct panfrost_perfcnt_job_ctx *prev_ctx, *new_ctx;
	struct panfrost_device *pfdev = job->pfdev;
	unsigned int i;

	new_ctx = job->perfcnt_ctx;
	prev_ctx = pfdev->perfcnt->last_ctx;
	if (!prev_ctx)
		return false;

	if (!refcount_inc_not_zero(&prev_ctx->refcount))
		return false;

	if (!panfrost_perfcnt_job_ctx_cmp(prev_ctx, new_ctx)) {
		refcount_dec(&prev_ctx->refcount);
		return false;
	}

	/*
	 * Make sure we increment busycnt, as panfrost_perfcnt_put_job_ctx()
	 * will decrement it.
	 */
	for (i = 0; i < prev_ctx->perfmon_count; i++)
		atomic_inc(&prev_ctx->perfmons[i]->busycnt);

	panfrost_perfcnt_put_job_ctx(new_ctx);
	job->perfcnt_ctx = prev_ctx;
	job->perfcnt_fence = dma_fence_get(prev_ctx->wait_fence);
	return true;
}

int panfrost_perfcnt_push_job(struct panfrost_job *job)
{
	struct panfrost_perfcnt_job_ctx *prev_ctx, *new_ctx;
	struct panfrost_device *pfdev = job->pfdev;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&pfdev->perfcnt->ctx_lock, flags);
	new_ctx = job->perfcnt_ctx;
	prev_ctx = pfdev->perfcnt->last_ctx;
	/*
	 * In order to keep things relatively fast even when HW counters are
	 * enabled we try to avoid having to dump perfcounters at the end of
	 * each job (which implies making other jobs wait for this dump to
	 * finish) when that's possible.
	 * This is only acceptable if all queued jobs share the same perfctx,
	 * that is, they have the same list of jobs attached to them. In this
	 * condition we are guaranteed that nothing will increment the counters
	 * behind our back.
	 */
	if (panfrost_perfcnt_try_reuse_last_job_ctx(job))
		goto out;

	new_ctx->done_fence = panfrost_perfcnt_fence_create(pfdev);
	if (IS_ERR(new_ctx->done_fence)) {
		ret = PTR_ERR(new_ctx->done_fence);
		goto out;
	}

	/*
	 * The previous job has a different perfmon ctx, so we must wait for it
	 * to be done dumping the counters before we can schedule this new job,
	 * otherwise we might corrupt the counter values.
	 */
	if (prev_ctx)
		new_ctx->wait_fence = dma_fence_get(prev_ctx->done_fence);

	job->perfcnt_fence = dma_fence_get(new_ctx->wait_fence);
	pfdev->perfcnt->last_ctx = new_ctx;

out:
	spin_unlock_irqrestore(&pfdev->perfcnt->ctx_lock, flags);
	return ret;
}

int panfrost_perfcnt_init(struct panfrost_device *pfdev)
{
	struct panfrost_perfcnt *perfcnt;
	struct drm_gem_shmem_object *bo;
	size_t size;
	u32 status;
	int ret;

	if (panfrost_has_hw_feature(pfdev, HW_FEATURE_V4)) {
		unsigned int ncoregroups;

		ncoregroups = hweight64(pfdev->features.l2_present);
		size = ncoregroups * BLOCKS_PER_COREGROUP *
		       COUNTERS_PER_BLOCK * BYTES_PER_COUNTER;
	} else {
		unsigned int nl2c, ncores;

		/*
		 * TODO: define a macro to extract the number of l2 caches from
		 * mem_features.
		 */
		nl2c = ((pfdev->features.mem_features >> 8) & GENMASK(3, 0)) + 1;

		/*
		 * The ARM driver is grouping cores per core group and then
		 * only using the number of cores in group 0 to calculate the
		 * size. Not sure why this is done like that, but I guess
		 * shader_present will only show cores in the first group
		 * anyway.
		 */
		ncores = hweight64(pfdev->features.shader_present);

		/*
		 * There's always one JM and one Tiler block, hence the '+ 2'
		 * here.
		 */
		size = (nl2c + ncores + 2) *
		       COUNTERS_PER_BLOCK * BYTES_PER_COUNTER;
	}

	perfcnt = devm_kzalloc(pfdev->dev, sizeof(*perfcnt), GFP_KERNEL);
	if (!perfcnt)
		return -ENOMEM;

	bo = drm_gem_shmem_create(pfdev->ddev, size);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	perfcnt->bo = to_panfrost_bo(&bo->base);

	/*
	 * We always use the same buffer, so let's map it once and keep it
	 * mapped until the driver is unloaded. This might be a problem if
	 * we start using different AS and the perfcnt BO is not mapped at
	 * the same GPU virtual address.
	 */
	ret = panfrost_mmu_map(perfcnt->bo);
	if (ret)
		goto err_put_bo;

	/* Disable everything. */
	gpu_write(pfdev, GPU_PERFCNT_CFG,
		  GPU_PERFCNT_CFG_AS(0) |
		  GPU_PERFCNT_CFG_MODE(GPU_PERFCNT_CFG_MODE_OFF) |
		  (panfrost_model_cmp(pfdev, 0x1000) >= 0 ?
		   GPU_PERFCNT_CFG_SETSEL(1) : 0));
	gpu_write(pfdev, GPU_PRFCNT_JM_EN, 0);
	gpu_write(pfdev, GPU_PRFCNT_SHADER_EN, 0);
	gpu_write(pfdev, GPU_PRFCNT_MMU_L2_EN, 0);
	gpu_write(pfdev, GPU_PRFCNT_TILER_EN, 0);

	perfcnt->buf = drm_gem_vmap(&bo->base);
	if (IS_ERR(perfcnt->buf)) {
		ret = PTR_ERR(perfcnt->buf);
		goto err_put_bo;
	}

	INIT_WORK(&perfcnt->dumpwork, panfrost_perfcnt_dump_work);
	mutex_init(&perfcnt->cfg_lock);
	spin_lock_init(&perfcnt->fence_lock);
	spin_lock_init(&perfcnt->ctx_lock);
	perfcnt->fence_context = dma_fence_context_alloc(1);
	pfdev->perfcnt = perfcnt;

	/*
	 * Invalidate the cache and clear the counters to start from a fresh
	 * state.
	 */
	gpu_write(pfdev, GPU_INT_MASK, 0);
	gpu_write(pfdev, GPU_INT_CLEAR, GPU_IRQ_CLEAN_CACHES_COMPLETED);

	gpu_write(pfdev, GPU_CMD, GPU_CMD_PERFCNT_CLEAR);
	gpu_write(pfdev, GPU_CMD, GPU_CMD_CLEAN_INV_CACHES);
	ret = readl_relaxed_poll_timeout(pfdev->iomem + GPU_INT_RAWSTAT,
					 status,
					 status &
					 GPU_IRQ_CLEAN_CACHES_COMPLETED,
					 100, 10000);
	if (ret)
		goto err_gem_vunmap;

	gpu_write(pfdev, GPU_INT_MASK, GPU_IRQ_MASK_ALL);

	return 0;

err_gem_vunmap:
	drm_gem_vunmap(&pfdev->perfcnt->bo->base.base, pfdev->perfcnt->buf);

err_put_bo:
	drm_gem_object_put_unlocked(&bo->base);
	return ret;
}

void panfrost_perfcnt_fini(struct panfrost_device *pfdev)
{
	drm_gem_vunmap(&pfdev->perfcnt->bo->base.base, pfdev->perfcnt->buf);
	drm_gem_object_put_unlocked(&pfdev->perfcnt->bo->base.base);
}
