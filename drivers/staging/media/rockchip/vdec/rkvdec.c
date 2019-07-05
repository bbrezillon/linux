// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder driver
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *
 * Based on rkvdec driver by Google LLC. (Tomasz Figa <tfiga@chromium.org>)
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-mem2mem-h264-codec.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include "rkvdec.h"
#include "rkvdec-regs.h"

static const struct v4l2_m2m_codec_decoded_fmt_desc rkvdec_decoded_fmts[] = {
	{ .fourcc = V4L2_PIX_FMT_NV12 },
};

static const V4L2_M2M_CODEC_CTRLS(rkvdec_h264_ctrls,
		V4L2_M2M_H264_DEC_DECODE_PARAMS_CTRL,
		V4L2_M2M_H264_DEC_SLICE_PARAMS_CTRL,
		V4L2_M2M_H264_DEC_SPS_CTRL,
		V4L2_M2M_H264_DEC_PPS_CTRL,
		V4L2_M2M_H264_DEC_SCALING_MATRIX_CTRL,
		V4L2_M2M_H264_DEC_MODE_CTRL(V4L2_MPEG_VIDEO_H264_SLICE_BASED_DECODING,
					    V4L2_MPEG_VIDEO_H264_FRAME_BASED_DECODING));

static const struct v4l2_m2m_codec_coded_fmt_desc rkvdec_coded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE_RAW,
		.requires_requests = true,
		.frmsize = &((struct v4l2_frmsize_stepwise){
			.min_width = 48,
			.max_width = 3840,
			.step_width = 16,
			.min_height = 48,
			.max_height = 2160,
			.step_height = 16,
		}),
		.ctrls = &rkvdec_h264_ctrls,
		.ops = &rkvdec_h264_fmt_ops,
	}
};

static int rkvdec_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct rkvdec_dev *rkvdec = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, rkvdec->dev->driver->name,
		sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 rkvdec->dev->driver->name);
	return 0;
}

static int
rkvdec_s_output_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct rkvdec_ctx *ctx = fh_to_rkvdec_ctx(priv);
	struct v4l2_m2m_ctx *m2m_ctx = v4l2_m2m_codec_get_m2m_ctx(&ctx->base);
	struct vb2_queue *peer_vq;
	int ret;


	/*
	 * Since format change on the OUTPUT queue will reset the CAPTURE
	 * queue, we can't allow doing so when the CAPTURE queue has buffers
	 * allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq))
		return -EBUSY;

	ret = v4l2_m2m_codec_s_output_fmt(file, priv, f);
	if (ret)
		return ret;

	return 0;
}

const struct v4l2_ioctl_ops rkvdec_ioctl_ops = {
	.vidioc_querycap = rkvdec_querycap,
	.vidioc_enum_framesizes = v4l2_m2m_codec_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = v4l2_m2m_codec_try_capture_fmt,
	.vidioc_try_fmt_vid_out_mplane = v4l2_m2m_codec_try_output_fmt,
	.vidioc_s_fmt_vid_out_mplane = rkvdec_s_output_fmt,
	.vidioc_s_fmt_vid_cap_mplane = v4l2_m2m_codec_s_capture_fmt,
	.vidioc_g_fmt_vid_out_mplane = v4l2_m2m_codec_g_output_fmt,
	.vidioc_g_fmt_vid_cap_mplane = v4l2_m2m_codec_g_capture_fmt,
	.vidioc_enum_fmt_vid_out = v4l2_m2m_codec_enum_output_fmt,
	.vidioc_enum_fmt_vid_cap = v4l2_m2m_codec_enum_capture_fmt,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static int rkvdec_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			      unsigned int *num_planes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct v4l2_m2m_codec_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pixfmt;
	int ret;

	ret = v4l2_m2m_codec_queue_setup(vq, num_buffers, num_planes, sizes,
					 alloc_devs);
	if (ret)
		return ret;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		return 0;

	pixfmt = &ctx->decoded_fmt.fmt.pix_mp;
	sizes[0] += 128 * DIV_ROUND_UP(pixfmt->width, 16) *
		    DIV_ROUND_UP(pixfmt->height, 16);
	return 0;
}

const struct vb2_ops rkvdec_queue_ops = {
	.queue_setup = rkvdec_queue_setup,
	.buf_prepare = v4l2_m2m_codec_buf_prepare,
	.buf_queue = v4l2_m2m_codec_buf_queue,
	.buf_out_validate = v4l2_m2m_codec_buf_out_validate,
	.buf_request_complete = v4l2_m2m_codec_buf_request_complete,
	.start_streaming = v4l2_m2m_codec_start_streaming,
	.stop_streaming = v4l2_m2m_codec_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static const struct media_device_ops rkvdec_media_ops = {
	.req_validate = v4l2_m2m_codec_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static void rkvdec_job_finish_no_pm(struct rkvdec_ctx *ctx,
				    enum vb2_buffer_state result)
{
	struct v4l2_m2m_ctx *m2m_ctx = v4l2_m2m_codec_get_m2m_ctx(&ctx->base);
	struct vb2_v4l2_buffer *dst_buf = v4l2_m2m_next_dst_buf(m2m_ctx);
	const struct v4l2_format *f;

	f = v4l2_m2m_codec_get_decoded_fmt(&ctx->base);
	if (result != VB2_BUF_STATE_ERROR)
		dst_buf->planes[0].bytesused = f->fmt.pix_mp.plane_fmt[0].sizeimage;
	else
		dst_buf->planes[0].bytesused = 0;
	v4l2_m2m_codec_job_finish(&ctx->base, result);
}

static void rkvdec_job_finish(struct rkvdec_ctx *ctx, enum vb2_buffer_state result)
{
	struct rkvdec_dev *rkvdec = codec_to_rkvdec(ctx->base.codec);

	pm_runtime_mark_last_busy(rkvdec->dev);
	pm_runtime_put_autosuspend(rkvdec->dev);
	rkvdec_job_finish_no_pm(ctx, result);
}

static void rkvdec_device_run(void *priv)
{
	struct rkvdec_ctx *ctx = codec_ctx_to_rkvdec_ctx(priv);
	struct rkvdec_dev *rkvdec = codec_to_rkvdec(ctx->base.codec);
	int ret;

	ret = pm_runtime_get_sync(rkvdec->dev);
	if (ret < 0) {
		rkvdec_job_finish_no_pm(ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	ret = v4l2_m2m_codec_device_run(priv);
	if (ret < 0)
		rkvdec_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

static struct v4l2_m2m_ops rkvdec_m2m_ops = {
	.device_run = rkvdec_device_run,
};

static const struct v4l2_m2m_codec_caps rkvdec_codec_caps = {
	V4L2_M2M_CODEC_CODED_FMTS(rkvdec_coded_fmts),
	V4L2_M2M_CODEC_DECODED_FMTS(rkvdec_decoded_fmts),
};

static int rkvdec_queue_init(struct v4l2_m2m_codec_ctx *codec_ctx,
			     struct vb2_queue *src_vq,
			     struct vb2_queue *dst_vq)
{
	struct rkvdec_ctx *ctx = codec_ctx_to_rkvdec_ctx(codec_ctx);
	struct rkvdec_dev *rkvdec = codec_to_rkvdec(codec_ctx->codec);
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rkvdec_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;

	/*
	 * Driver does mostly sequential access, so sacrifice TLB efficiency
	 * for faster allocation. Also, no CPU access on the source queue,
	 * so no kernel mapping needed.
	 */
	src_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &rkvdec->vdev_lock;
	src_vq->dev = rkvdec->v4l2_dev.dev;
	src_vq->supports_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->bidirectional = true;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rkvdec_queue_ops;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &rkvdec->vdev_lock;
	dst_vq->dev = rkvdec->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static const struct v4l2_m2m_codec_ops rkvdec_codec_ops = {
	.queue_init = rkvdec_queue_init,
};

static int rkvdec_open(struct file *filp)
{
	struct rkvdec_dev *rkvdec = video_drvdata(filp);
	struct rkvdec_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = v4l2_m2m_codec_ctx_init(&ctx->base, filp, &rkvdec->codec);
	if (ret)
		goto err_free_ctx;

	return 0;

err_free_ctx:
	kfree(ctx);
	return ret;
}

static int rkvdec_release(struct file *filp)
{
	struct v4l2_m2m_codec_ctx *codec_ctx = file_to_v4l2_m2m_codec_ctx(filp);
	struct rkvdec_ctx *ctx = codec_ctx_to_rkvdec_ctx(codec_ctx);

	v4l2_m2m_codec_ctx_cleanup(codec_ctx);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations rkvdec_fops = {
	.owner = THIS_MODULE,
	.open = rkvdec_open,
	.release = rkvdec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int rkvdec_v4l2_init(struct rkvdec_dev *rkvdec)
{
	struct v4l2_m2m_dev *m2m_dev;
	struct video_device *vdev;
	int ret;

	ret = v4l2_device_register(rkvdec->dev, &rkvdec->v4l2_dev);
	if (ret) {
		dev_err(rkvdec->dev, "Failed to register V4L2 device\n");
		return ret;
	}

	m2m_dev = v4l2_m2m_init(&rkvdec_m2m_ops);
	if (IS_ERR(m2m_dev)) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(m2m_dev);
		goto err_unregister_v4l2;
	}

	rkvdec->mdev.dev = rkvdec->dev;
	strscpy(rkvdec->mdev.model, "rkvdec", sizeof(rkvdec->mdev.model));
	strscpy(rkvdec->mdev.bus_info, "platform:rkvdec",
		sizeof(rkvdec->mdev.bus_info));
	media_device_init(&rkvdec->mdev);
	rkvdec->mdev.ops = &rkvdec_media_ops;
	rkvdec->v4l2_dev.mdev = &rkvdec->mdev;

	ret = v4l2_m2m_codec_init(&rkvdec->codec, V4L2_M2M_DECODER, m2m_dev,
				  &rkvdec->v4l2_dev, &rkvdec_codec_caps,
				  &rkvdec_codec_ops, &rkvdec_fops,
				  &rkvdec_ioctl_ops, &rkvdec->vdev_lock,
				  "rkvdec", rkvdec);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to init codec object\n");
		goto err_cleanup_mc;
	}

	vdev = v4l2_m2m_codec_to_vdev(&rkvdec->codec);
	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to register video device\n");
		goto err_cleanup_mc;
	}

	ret = v4l2_m2m_register_media_controller(m2m_dev, vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev,
			 "Failed to initialize V4L2 M2M media controller\n");
		goto err_unregister_vdev;
	}

	ret = media_device_register(&rkvdec->mdev);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to register media device\n");
		goto err_unregister_mc;
	}

	return 0;

err_unregister_mc:
	v4l2_m2m_unregister_media_controller(m2m_dev);

err_unregister_vdev:
	video_unregister_device(vdev);

err_cleanup_mc:
	media_device_cleanup(&rkvdec->mdev);
	v4l2_m2m_release(m2m_dev);

err_unregister_v4l2:
	v4l2_device_unregister(&rkvdec->v4l2_dev);
	return ret;
}

static void rkvdec_v4l2_cleanup(struct rkvdec_dev *rkvdec)
{
	media_device_unregister(&rkvdec->mdev);
	v4l2_m2m_unregister_media_controller(rkvdec->codec.m2m_dev);
	video_unregister_device(v4l2_m2m_codec_to_vdev(&rkvdec->codec));
	media_device_cleanup(&rkvdec->mdev);
	v4l2_m2m_release(rkvdec->codec.m2m_dev);
	v4l2_device_unregister(&rkvdec->v4l2_dev);
}

static irqreturn_t rkvdec_irq_handler(int irq, void *priv)
{
	struct rkvdec_dev *rkvdec = priv;
	u32 status = readl(rkvdec->regs + RKVDEC_REG_INTERRUPT);

	dev_dbg(rkvdec->dev, "dec status %x\n", status);

	writel(0, rkvdec->regs + RKVDEC_REG_INTERRUPT);

	if (cancel_delayed_work(&rkvdec->watchdog_work)) {
		struct v4l2_m2m_codec_ctx *codec_ctx;

		codec_ctx = v4l2_m2m_get_curr_priv(rkvdec->codec.m2m_dev);
		rkvdec_job_finish(codec_ctx_to_rkvdec_ctx(codec_ctx),
				  VB2_BUF_STATE_DONE);
	}

	return IRQ_HANDLED;
}

static void rkvdec_watchdog_func(struct work_struct *work)
{
	struct v4l2_m2m_codec_ctx *codec_ctx;
	struct rkvdec_dev *rkvdec;

	rkvdec = container_of(to_delayed_work(work), struct rkvdec_dev,
			      watchdog_work);
	codec_ctx = v4l2_m2m_get_curr_priv(rkvdec->codec.m2m_dev);
	if (codec_ctx) {
		dev_err(rkvdec->dev, "Frame processing timed out!\n");
		writel(RKVDEC_IRQ_DIS, rkvdec->regs + RKVDEC_REG_INTERRUPT);
		writel(0, rkvdec->regs + RKVDEC_REG_SYSCTRL);
		rkvdec_job_finish(codec_ctx_to_rkvdec_ctx(codec_ctx),
				  VB2_BUF_STATE_ERROR);
	}
}

static const struct of_device_id of_rkvdec_match[] = {
	{ .compatible = "rockchip,rk3399-vdec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_rkvdec_match);

static const char *rkvdec_clk_names[] = {
	"aclk", "iface", "cabac", "core"
};

static int rkvdec_probe(struct platform_device *pdev)
{
	struct rkvdec_dev *rkvdec;
	struct resource *res;
	unsigned int i;
	int ret, irq;

	rkvdec = devm_kzalloc(&pdev->dev, sizeof(*rkvdec), GFP_KERNEL);
	if (!rkvdec)
		return -ENOMEM;

	platform_set_drvdata(pdev, rkvdec);
	rkvdec->dev = &pdev->dev;
	mutex_init(&rkvdec->vdev_lock);
	INIT_DELAYED_WORK(&rkvdec->watchdog_work, rkvdec_watchdog_func);

	rkvdec->clocks = devm_kcalloc(&pdev->dev, ARRAY_SIZE(rkvdec_clk_names),
				      sizeof(*rkvdec->clocks), GFP_KERNEL);
	if (!rkvdec->clocks)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(rkvdec_clk_names); i++)
		rkvdec->clocks[i].id = rkvdec_clk_names[i];

	ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(rkvdec_clk_names),
				rkvdec->clocks);
	if (ret)
		return ret;

	/*
	 * Bump ACLK to max. possible freq. (500 MHz) to improve performance
	 * When 4k video playback.
	 */
	clk_set_rate(rkvdec->clocks[0].clk, 500 * 1000 * 1000);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	rkvdec->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rkvdec->regs))
		return PTR_ERR(rkvdec->regs);

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Could not set DMA coherent mask.\n");
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0) {
		dev_err(&pdev->dev, "Could not get vdec IRQ\n");
		return -ENXIO;
	}

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					rkvdec_irq_handler, IRQF_ONESHOT,
					dev_name(&pdev->dev), rkvdec);
	if (ret) {
		dev_err(&pdev->dev, "Could not request vdec IRQ\n");
		return ret;
	}

	pm_runtime_set_autosuspend_delay(&pdev->dev, 100);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = rkvdec_v4l2_init(rkvdec);
	if (ret)
		goto err_disable_runtime_pm;

	return 0;

err_disable_runtime_pm:
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int rkvdec_remove(struct platform_device *pdev)
{
	struct rkvdec_dev *rkvdec = platform_get_drvdata(pdev);

	rkvdec_v4l2_cleanup(rkvdec);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	return 0;
}

#ifdef CONFIG_PM
static int rkvdec_runtime_resume(struct device *dev)
{
	struct rkvdec_dev *rkvdec = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(ARRAY_SIZE(rkvdec_clk_names), rkvdec->clocks);
}

static int rkvdec_runtime_suspend(struct device *dev)
{
	struct rkvdec_dev *rkvdec = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(ARRAY_SIZE(rkvdec_clk_names), rkvdec->clocks);
	return 0;
}
#endif

static const struct dev_pm_ops rkvdec_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rkvdec_runtime_suspend, rkvdec_runtime_resume, NULL)
};

static struct platform_driver rkvdec_driver = {
	.probe = rkvdec_probe,
	.remove = rkvdec_remove,
	.driver = {
		   .name = "rkvdec",
		   .of_match_table = of_match_ptr(of_rkvdec_match),
		   .pm = &rkvdec_pm_ops,
	},
};
module_platform_driver(rkvdec_driver);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@collabora.com>");
MODULE_DESCRIPTION("Rockchip Video Decoder driver");
MODULE_LICENSE("GPL v2");
