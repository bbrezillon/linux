// SPDX-License-Identifier: GPL-2.0
/* Copyright 2019 Collabora Ltd */

#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/panfrost_drm.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
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

struct panfrost_perfcnt {
	struct panfrost_gem_object *bo;
	size_t bosize;
	void *buf;
	bool enabled;
	struct mutex lock;
	struct completion dump_comp;
};



void panfrost_perfcnt_clean_cache_done(struct panfrost_device *pfdev)
{
	complete(&pfdev->perfcnt->dump_comp);
}

void panfrost_perfcnt_sample_done(struct panfrost_device *pfdev)
{
	gpu_write(pfdev, GPU_CMD, GPU_CMD_CLEAN_CACHES);
}

static void panfrost_perfcnt_setup(struct panfrost_device *pfdev)
{
	u32 cfg;

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

	if (!pfdev->perfcnt->enabled)
		return;

	gpu_write(pfdev, GPU_PRFCNT_JM_EN, 0xffffffff);
	gpu_write(pfdev, GPU_PRFCNT_SHADER_EN, 0xffffffff);
	gpu_write(pfdev, GPU_PRFCNT_MMU_L2_EN, 0xffffffff);

	/*
	 * Due to PRLAM-8186 we need to disable the Tiler before we enable HW
	 * counters.
	 */
	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_8186))
		gpu_write(pfdev, GPU_PRFCNT_TILER_EN, 0);
	else
		gpu_write(pfdev, GPU_PRFCNT_TILER_EN, 0xffffffff);

	gpu_write(pfdev, GPU_PERFCNT_CFG,
		  cfg | GPU_PERFCNT_CFG_MODE(GPU_PERFCNT_CFG_MODE_MANUAL));

	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_8186))
		gpu_write(pfdev, GPU_PRFCNT_TILER_EN, 0xffffffff);
}

static int panfrost_perfcnt_dump(struct panfrost_device *pfdev)
{
	u64 gpuva;
	int ret;

	reinit_completion(&pfdev->perfcnt->dump_comp);
	gpuva = pfdev->perfcnt->bo->node.start << PAGE_SHIFT;
	gpu_write(pfdev, GPU_PERFCNT_BASE_LO, gpuva);
	gpu_write(pfdev, GPU_PERFCNT_BASE_HI, gpuva >> 32);
	gpu_write(pfdev, GPU_CMD, GPU_CMD_PERFCNT_SAMPLE);
	ret = wait_for_completion_interruptible_timeout(&pfdev->perfcnt->dump_comp,
							msecs_to_jiffies(1000));
	if (!ret)
		ret = -ETIMEDOUT;
	else if (ret > 0)
		ret = 0;

	return ret;
}

void panfrost_perfcnt_resume(struct panfrost_device *pfdev)
{
	if (pfdev->perfcnt)
		panfrost_perfcnt_setup(pfdev);
}

static ssize_t panfrost_perfcnt_enable_read(struct file *file,
					    char __user *user_buf,
					    size_t count, loff_t *ppos)
{
	struct panfrost_device *pfdev = file->private_data;
	ssize_t ret;

	mutex_lock(&pfdev->perfcnt->lock);
	ret = simple_read_from_buffer(user_buf, count, ppos,
				      pfdev->perfcnt->enabled ? "Y\n" : "N\n",
				      2);
	mutex_unlock(&pfdev->perfcnt->lock);

	return ret;
}

static ssize_t panfrost_perfcnt_enable_write(struct file *file,
					     const char __user *user_buf,
					     size_t count, loff_t *ppos)
{
	struct panfrost_device *pfdev = file->private_data;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(user_buf, count, &enable);
	if (ret)
		return ret;

	ret = pm_runtime_get_sync(pfdev->dev);
	if (ret < 0)
		return ret;

	mutex_lock(&pfdev->perfcnt->lock);
	if (enable != pfdev->perfcnt->enabled) {
		pfdev->perfcnt->enabled = enable;
		panfrost_perfcnt_setup(pfdev);
	}
	mutex_unlock(&pfdev->perfcnt->lock);

	pm_runtime_mark_last_busy(pfdev->dev);
	pm_runtime_put_autosuspend(pfdev->dev);

	return count;
}

static const struct file_operations panfrost_perfcnt_enable_fops = {
	.read = panfrost_perfcnt_enable_read,
	.write = panfrost_perfcnt_enable_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static ssize_t panfrost_perfcnt_dump_read(struct file *file,
					  char __user *user_buf,
					  size_t count, loff_t *ppos)
{
	struct panfrost_device *pfdev = file->private_data;
	ssize_t ret;

	ret = pm_runtime_get_sync(pfdev->dev);
	if (ret < 0)
		return ret;

	mutex_lock(&pfdev->perfcnt->lock);
	if (!pfdev->perfcnt->enabled) {
		ret = -EINVAL;
		goto out;
	}

	ret = panfrost_perfcnt_dump(pfdev);
	if (ret)
		goto out;

	ret = simple_read_from_buffer(user_buf, count, ppos,
				      pfdev->perfcnt->buf,
				      pfdev->perfcnt->bosize);

out:
	mutex_unlock(&pfdev->perfcnt->lock);
	pm_runtime_mark_last_busy(pfdev->dev);
	pm_runtime_put_autosuspend(pfdev->dev);

	return ret;
}

static const struct file_operations panfrost_perfcnt_dump_fops = {
	.read = panfrost_perfcnt_dump_read,
	.open = simple_open,
	.llseek = default_llseek,
};

int panfrost_perfcnt_debugfs_init(struct drm_minor *minor)
{
	struct panfrost_device *pfdev = to_panfrost_device(minor->dev);
	struct dentry *file, *dir;

	dir = debugfs_create_dir("perfcnt", minor->debugfs_root);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	file = debugfs_create_file("dump", S_IRUSR, dir, pfdev,
				   &panfrost_perfcnt_dump_fops);
	if (IS_ERR(file))
		return PTR_ERR(file);

	file = debugfs_create_file("enable", S_IRUSR | S_IWUSR, dir, pfdev,
				   &panfrost_perfcnt_enable_fops);
	if (IS_ERR(file))
		return PTR_ERR(file);

	return 0;
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
	perfcnt->bosize = size;

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

	init_completion(&perfcnt->dump_comp);
	mutex_init(&perfcnt->lock);
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
