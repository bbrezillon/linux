/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2019 Collabora Ltd */
#ifndef __PANFROST_PERFCNT_H__
#define __PANFROST_PERFCNT_H__

#include <linux/bitops.h>

struct panfrost_perfcnt_job_ctx;

#define PERFCNT(_shader, _tiler, _mmu_l2, _jm)		\
	{ _shader, _tiler, _mmu_l2, _jm }
#define NO_PERFCNT      PERFCNT(0, 0, 0, 0)

/* FIXME: Declare counters for all models */
#define hw_perfcnt_t600	NO_PERFCNT
#define hw_perfcnt_t620	NO_PERFCNT
#define hw_perfcnt_t720	NO_PERFCNT
#define hw_perfcnt_t760	NO_PERFCNT
#define hw_perfcnt_t820	NO_PERFCNT
#define hw_perfcnt_t830	NO_PERFCNT
#define hw_perfcnt_t860	NO_PERFCNT
#define hw_perfcnt_t880	NO_PERFCNT
#define hw_perfcnt_g76	NO_PERFCNT
#define hw_perfcnt_g71	NO_PERFCNT
#define hw_perfcnt_g72	NO_PERFCNT
#define hw_perfcnt_g51	NO_PERFCNT
#define hw_perfcnt_g52	NO_PERFCNT
#define hw_perfcnt_g31	NO_PERFCNT

void panfrost_perfcnt_sample_done(struct panfrost_device *pfdev);
void panfrost_perfcnt_clean_cache_done(struct panfrost_device *pfdev);
int panfrost_perfcnt_push_job(struct panfrost_job *job);
void panfrost_perfcnt_run_job(struct panfrost_job *job);
void panfrost_perfcnt_finish_job(struct panfrost_job *job,
				 bool skip_dump);
void panfrost_perfcnt_clean_job_ctx(struct panfrost_job *job);
int panfrost_perfcnt_create_job_ctx(struct panfrost_job *job,
				    struct drm_file *file_priv,
				    struct drm_panfrost_submit *args);
void panfrost_perfcnt_open(struct panfrost_file_priv *pfile);
void panfrost_perfcnt_close(struct panfrost_file_priv *pfile);
int panfrost_perfcnt_init(struct panfrost_device *pfdev);
void panfrost_perfcnt_fini(struct panfrost_device *pfdev);

int panfrost_ioctl_get_perfcnt_layout(struct drm_device *dev, void *data,
				      struct drm_file *file_priv);
int panfrost_ioctl_create_perfmon(struct drm_device *dev, void *data,
				  struct drm_file *file_priv);
int panfrost_ioctl_destroy_perfmon(struct drm_device *dev, void *data,
				   struct drm_file *file_priv);
int panfrost_ioctl_get_perfmon_values(struct drm_device *dev, void *data,
				      struct drm_file *file_priv);

#endif
