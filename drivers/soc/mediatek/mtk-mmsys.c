// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk-mmsys.h>

struct mtk_mmsys_driver_data {
	const char *clk_driver;
	const char *mmsys_driver;
};

struct mtk_mmsys_private_data {
	void __iomem *config_regs;
	struct mtk_mmsys_conn_funcs *funcs;
};

static const struct mtk_mmsys_driver_data mt2701_mmsys_driver_data = {
	.clk_driver = "clk-mt2701-mm",
	.mmsys_driver = "mt2701-mmsys",
};

static const struct mtk_mmsys_driver_data mt2712_mmsys_driver_data = {
	.clk_driver = "clk-mt2712-mm",
	.mmsys_driver = "mt2701-mmsys",
};

static const struct mtk_mmsys_driver_data mt6779_mmsys_driver_data = {
	.clk_driver = "clk-mt6779-mm",
	.mmsys_driver = "mt2701-mmsys",
};

static const struct mtk_mmsys_driver_data mt6797_mmsys_driver_data = {
	.clk_driver = "clk-mt6797-mm",
	.mmsys_driver = "mt2701-mmsys",
};

static const struct mtk_mmsys_driver_data mt8173_mmsys_driver_data = {
	.clk_driver = "clk-mt8173-mm",
	.mmsys_driver = "mt2701-mmsys",
};

static const struct mtk_mmsys_driver_data mt8183_mmsys_driver_data = {
	.clk_driver = "clk-mt8183-mm",
};

void mtk_mmsys_ddp_connect(struct device *dev,
			   enum mtk_ddp_comp_id cur,
			   enum mtk_ddp_comp_id next)
{
	struct mtk_mmsys_private_data *private = dev_get_drvdata(dev);
	void __iomem *config_regs = private->config_regs;
	struct mtk_mmsys_conn_funcs *priv_funcs = private->funcs;
	unsigned int addr, value, reg;

	value = priv_funcs->mout_en(cur, next, &addr);
	if (value) {
		reg = readl_relaxed(config_regs + addr) | value;
		writel_relaxed(reg, config_regs + addr);
	}

	priv_funcs->sout_sel(config_regs, cur, next);

	value = priv_funcs->sel_in(cur, next, &addr);
	if (value) {
		reg = readl_relaxed(config_regs + addr) | value;
		writel_relaxed(reg, config_regs + addr);
	}
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_connect);

void mtk_mmsys_ddp_disconnect(struct device *dev,
			      enum mtk_ddp_comp_id cur,
			      enum mtk_ddp_comp_id next)
{
	struct mtk_mmsys_private_data *private = dev_get_drvdata(dev);
	void __iomem *config_regs = private->config_regs;
	struct mtk_mmsys_conn_funcs *priv_funcs = private->funcs;
	unsigned int addr, value, reg;

	value = priv_funcs->mout_en(cur, next, &addr);
	if (value) {
		reg = readl_relaxed(config_regs + addr) & ~value;
		writel_relaxed(reg, config_regs + addr);
	}

	value = priv_funcs->sel_in(cur, next, &addr);
	if (value) {
		reg = readl_relaxed(config_regs + addr) & ~value;
		writel_relaxed(reg, config_regs + addr);
	}
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_disconnect);

void mtk_mmsys_register_conn_funcs(struct device *dev,
				   struct mtk_mmsys_conn_funcs *funcs)
{
	struct mtk_mmsys_private_data *private = dev_get_drvdata(dev);

	private->funcs = funcs;
}

static int mtk_mmsys_probe(struct platform_device *pdev)
{
	const struct mtk_mmsys_driver_data *data;
	struct device *dev = &pdev->dev;
	struct platform_device *clks;
	struct platform_device *drm;
	struct platform_device *mm;
	void __iomem *config_regs;
	struct resource *mem;
	int ret;
	struct mtk_mmsys_private_data *private;

	private = devm_kzalloc(dev, sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	config_regs = devm_ioremap_resource(dev, mem);
	if (IS_ERR(config_regs)) {
		ret = PTR_ERR(config_regs);
		dev_err(dev, "Failed to ioremap mmsys-config resource: %d\n",
			ret);
		return ret;
	}
	private->config_regs = config_regs;

	platform_set_drvdata(pdev, private);

	data = of_device_get_match_data(&pdev->dev);

	clks = platform_device_register_data(&pdev->dev, data->clk_driver,
					     PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(clks))
		return PTR_ERR(clks);

	mm = platform_device_register_data(&pdev->dev,
					   data->mmsys_driver,
					   PLATFORM_DEVID_AUTO,
					   NULL,
					   0);
	if (IS_ERR(mm))
		return PTR_ERR(mm);

	drm = platform_device_register_data(&pdev->dev, "mediatek-drm",
					    PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(drm)) {
		platform_device_unregister(clks);
		return PTR_ERR(drm);
	}

	return 0;
}

static const struct of_device_id of_match_mtk_mmsys[] = {
	{
		.compatible = "mediatek,mt2701-mmsys",
		.data = &mt2701_mmsys_driver_data,
	},
	{
		.compatible = "mediatek,mt2712-mmsys",
		.data = &mt2712_mmsys_driver_data,
	},
	{
		.compatible = "mediatek,mt6779-mmsys",
		.data = &mt6779_mmsys_driver_data,
	},
	{
		.compatible = "mediatek,mt6797-mmsys",
		.data = &mt6797_mmsys_driver_data,
	},
	{
		.compatible = "mediatek,mt8173-mmsys",
		.data = &mt8173_mmsys_driver_data,
	},
	{
		.compatible = "mediatek,mt8183-mmsys",
		.data = &mt8183_mmsys_driver_data,
	},
	{ }
};

static struct platform_driver mtk_mmsys_drv = {
	.driver = {
		.name = "mtk-mmsys",
		.of_match_table = of_match_mtk_mmsys,
	},
	.probe = mtk_mmsys_probe,
};

builtin_platform_driver(mtk_mmsys_drv);
