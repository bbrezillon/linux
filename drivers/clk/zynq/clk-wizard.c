// SPDX-License-Identifier: GPL-2.0
/*
 * Zynq Clocking Wizard driver
 *
 *  Copyright (C) 2018 Macronix
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#define SRR			0x0

#define SR			0x4
#define SR_LOCKED		BIT(0)

#define CCR(x)			(0x200 + ((x) * 4))

#define FBOUT_CFG		CCR(0)
#define FBOUT_DIV(x)		(x)
#define FBOUT_GET_DIV(x)	((x) & GENMASK(7, 0))
#define FBOUT_MUL(x)		((x) << 8)
#define FBOUT_GET_MUL(x)	(((x) & GENMASK(15, 8)) >> 8)
#define FBOUT_FRAC(x)		((x) << 16)
#define FBOUT_GET_FRAC(x)	(((x) & GENMASK(25, 16)) >> 16)
#define FBOUT_FRAC_EN		BIT(26)

#define FBOUT_PHASE		CCR(1)

#define OUT_CFG(x)		CCR(2 + ((x) * 3))
#define OUT_DIV(x)		(x)
#define OUT_GET_DIV(x)		((x) & GENMASK(7, 0))
#define OUT_FRAC(x)		((x) << 8)
#define OUT_GET_FRAC(x)		(((x) & GENMASK(17, 8)) >> 8)
#define OUT_FRAC_EN		BIT(18)

#define OUT_PHASE(x)		CCR(3 + ((x) * 3))
#define OUT_DUTY(x)		CCR(4 + ((x) * 3))

#define CTRL			CCR(23)
#define CTRL_SEN		BIT(2)
#define CTRL_SADDR		BIT(1)
#define CTRL_LOAD		BIT(0)

struct clkwzd;

struct clkwzd_fbout {
	struct clk_hw base;
	struct clkwzd *wzd;
};

static inline struct clkwzd_fbout *to_clkwzd_fbout(struct clk_hw *hw)
{
	return container_of(hw, struct clkwzd_fbout, base);
}

struct clkwzd_out {
	struct clk_hw base;
	struct clkwzd *wzd;
	unsigned int id;
};

static inline struct clkwzd_out *to_clkwzd_out(struct clk_hw *hw)
{
	return container_of(hw, struct clkwzd_out, base);
}

#define CLKWZD_MAX_OUTPUT	7

struct clkwzd {
	struct mutex lock;
	struct clk *aclk;
	struct clk *clk_in1;
	void __iomem *regs;
	struct clkwzd_out out[CLKWZD_MAX_OUTPUT];
	struct clkwzd_fbout fbout;
	struct clk_hw_onecell_data *onecell;
};


static int clkwzd_is_locked(struct clkwzd *wzd)
{
	bool prepared;

	mutex_lock(&wzd->lock);
	prepared = readl(wzd->regs + SR) & SR_LOCKED;
	mutex_unlock(&wzd->lock);

	return prepared;
}

static int clkwzd_apply_conf(struct clkwzd *wzd)
{
	int ret;
	u32 val;

	mutex_lock(&wzd->lock);
	ret = readl_poll_timeout(wzd->regs + SR, val, val & SR_LOCKED, 1, 100);
	if (!ret) {
		writel(CTRL_SEN | CTRL_SADDR | CTRL_LOAD, wzd->regs + CTRL);
		writel(CTRL_SADDR, wzd->regs + CTRL);
		ret = readl_poll_timeout(wzd->regs + SR, val, val & SR_LOCKED,
					 1, 100);
	}
	mutex_unlock(&wzd->lock);

	return 0;
}

static int clkwzd_fbout_is_prepared(struct clk_hw *hw)
{
	struct clkwzd_fbout *fbout = to_clkwzd_fbout(hw);

	return clkwzd_is_locked(fbout->wzd);
}

static int clkwzd_fbout_prepare(struct clk_hw *hw)
{
	struct clkwzd_fbout *fbout = to_clkwzd_fbout(hw);

	return clkwzd_apply_conf(fbout->wzd);
}

static unsigned long clkwzd_fbout_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct clkwzd_fbout *fbout = to_clkwzd_fbout(hw);
	unsigned long rate;
	u32 cfg;

	cfg = readl(fbout->wzd->regs + FBOUT_CFG);
	if (cfg & FBOUT_FRAC_EN)
		rate = DIV_ROUND_DOWN_ULL((u64)parent_rate *
					  ((FBOUT_GET_MUL(cfg) * 1000) +
					   FBOUT_GET_FRAC(cfg)),
					  1000);
	else
		rate = parent_rate * FBOUT_GET_MUL(cfg);

	rate /= FBOUT_GET_DIV(cfg);

	return rate;
}

static int clkwzd_fbout_set_phase(struct clk_hw *hw, int degrees)
{
	struct clkwzd_fbout *fbout = to_clkwzd_fbout(hw);

	writel(degrees * 1000, fbout->wzd->regs + FBOUT_PHASE);

	return 0;
}

static int clkwzd_fbout_get_phase(struct clk_hw *hw)
{
	struct clkwzd_fbout *fbout = to_clkwzd_fbout(hw);

	return readl(fbout->wzd->regs + FBOUT_PHASE) / 1000;
}

const struct clk_ops fbout_ops = {
	.is_prepared = clkwzd_fbout_is_prepared,
	.prepare = clkwzd_fbout_prepare,
	.recalc_rate = clkwzd_fbout_recalc_rate,
	.set_phase = clkwzd_fbout_set_phase,
	.get_phase = clkwzd_fbout_get_phase,
};

static int clkwzd_out_is_prepared(struct clk_hw *hw)
{
	struct clkwzd_out *out = to_clkwzd_out(hw);

	return clkwzd_is_locked(out->wzd);
}

static int clkwzd_out_prepare(struct clk_hw *hw)
{
	struct clkwzd_out *out = to_clkwzd_out(hw);

	return clkwzd_apply_conf(out->wzd);
}

static unsigned long clkwzd_out_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct clkwzd_out *out = to_clkwzd_out(hw);
	unsigned long rate;
	u32 cfg;

	cfg = readl(out->wzd->regs + OUT_CFG(out->id));
	if (cfg & OUT_FRAC_EN)
		rate = DIV_ROUND_DOWN_ULL((u64)parent_rate * 1000,
					  ((OUT_GET_DIV(cfg) * 1000) +
					   OUT_GET_FRAC(cfg)));
	else
		rate = parent_rate / OUT_GET_DIV(cfg);

	return rate;
}

static int clkwzd_out_set_rate(struct clk_hw *hw,
			       unsigned long rate,
			       unsigned long parent_rate)
{
	struct clkwzd_out *out = to_clkwzd_out(hw);
	u64 div;
	u32 cfg;


	div = DIV_ROUND_DOWN_ULL((u64)parent_rate * 1000, rate);
	if (div < 1000 || div > 255999)
		return -EINVAL;

	cfg = OUT_DIV((u32)div / 1000);

	if ((u32)div % 1000)
		cfg |= OUT_FRAC_EN | OUT_FRAC((u32)div % 1000);

	writel(cfg, out->wzd->regs + OUT_CFG(out->id));

	/* Set duty cycle to 50%. */
	writel(50000, out->wzd->regs + OUT_DUTY(out->id));

	return 0;
}

static long clkwzd_out_round_rate(struct clk_hw *hw,
				  unsigned long rate,
				  unsigned long *parent_rate)
{
	u64 div;

	div = DIV_ROUND_CLOSEST_ULL((u64)(*parent_rate) * 1000, rate);
	if (div < 1000)
		return *parent_rate;

	if (div > 255999)
		div = 255999;

	return DIV_ROUND_DOWN_ULL((u64)(*parent_rate) * 1000, (u32)div);
}

static int clkwzd_out_set_phase(struct clk_hw *hw, int degrees)
{
	struct clkwzd_out *out = to_clkwzd_out(hw);

	writel(degrees * 1000, out->wzd->regs + OUT_PHASE(out->id));

	return 0;
}

static int clkwzd_out_get_phase(struct clk_hw *hw)
{
	struct clkwzd_out *out = to_clkwzd_out(hw);

	return readl(out->wzd->regs + OUT_PHASE(out->id)) / 1000;
}

static const struct clk_ops out_ops = {
	.is_prepared = clkwzd_out_is_prepared,
	.prepare = clkwzd_out_prepare,
	.recalc_rate = clkwzd_out_recalc_rate,
	.round_rate = clkwzd_out_round_rate,
	.set_rate = clkwzd_out_set_rate,
	.set_phase = clkwzd_out_set_phase,
	.get_phase = clkwzd_out_get_phase,
};

static int zynq_clkwzd_probe(struct platform_device *pdev)
{
	struct clk_init_data fboutinit = { };
	const char *clk_in_name;
	struct resource *res;
	struct clkwzd *wzd;
	u32 i, noutputs = 0;
	int ret;

	wzd = devm_kzalloc(&pdev->dev, sizeof(*wzd), GFP_KERNEL);
	if (!wzd)
		return -ENOMEM;

	wzd->aclk = devm_clk_get(&pdev->dev, "aclk");
	if (IS_ERR(wzd->aclk))
		return PTR_ERR(wzd->aclk);

	wzd->clk_in1 = devm_clk_get(&pdev->dev, "clk_in1");
	if (IS_ERR(wzd->clk_in1))
		return PTR_ERR(wzd->clk_in1);

	of_property_read_u32(pdev->dev.of_node, "xlnx,clk-wizard-num-outputs",
			     &noutputs);
	if (!noutputs || noutputs >= CLKWZD_MAX_OUTPUT)
		return -EINVAL;

	wzd->onecell = devm_kzalloc(&pdev->dev,
				    sizeof(*wzd->onecell) +
				    (sizeof(*wzd->onecell->hws) * noutputs),
				    GFP_KERNEL);
	if (!wzd->onecell)
		return -ENOMEM;

	clk_in_name = __clk_get_name(wzd->clk_in1);
	if (!clk_in_name)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	wzd->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(wzd->regs))
		return PTR_ERR(wzd->regs);

	mutex_init(&wzd->lock);

	wzd->fbout.wzd = wzd;
	fboutinit.ops = &fbout_ops;
	fboutinit.flags = CLK_SET_RATE_GATE;
	fboutinit.num_parents = 1;
	fboutinit.parent_names = &clk_in_name;
	fboutinit.flags = CLK_SET_RATE_GATE;

	fboutinit.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s-fbout",
					dev_name(&pdev->dev));
	if (!fboutinit.name)
		return -ENOMEM;

	ret = clk_prepare_enable(wzd->aclk);
	if (ret)
		return ret;

	wzd->fbout.base.init = &fboutinit;
	ret = devm_clk_hw_register(&pdev->dev, &wzd->fbout.base);
	if (ret)
		goto err_disable_aclk;

	for (i = 0; i < noutputs; i++) {
		struct clk_init_data outinit = { };

		wzd->out[i].id = i;
		wzd->out[i].wzd = wzd;
		outinit.ops = &out_ops;
		outinit.num_parents = 1;
		outinit.parent_names = &fboutinit.name;
		outinit.flags = CLK_SET_RATE_GATE;
		wzd->out[i].base.init = &outinit;
		outinit.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					      "%s-out%d",
					      dev_name(&pdev->dev), i);
		if (!outinit.name) {
			ret = -ENOMEM;
			goto err_disable_aclk;
		}

		ret = devm_clk_hw_register(&pdev->dev, &wzd->out[i].base);
		if (ret)
			goto err_disable_aclk;

		wzd->onecell->hws[i] = &wzd->out[i].base;
	}

	wzd->onecell->num = noutputs;
	ret = devm_of_clk_add_hw_provider(&pdev->dev,
					  of_clk_hw_onecell_get,
					  wzd->onecell);
	if (ret)
		goto err_disable_aclk;

	platform_set_drvdata(pdev, wzd);

	return 0;

err_disable_aclk:
	clk_disable_unprepare(wzd->aclk);

	return ret;
}

static int zynq_clkwzd_remove(struct platform_device *pdev)
{
	struct clkwzd *wzd = platform_get_drvdata(pdev);

	clk_disable_unprepare(wzd->aclk);

	return 0;
}

static const struct of_device_id zynq_clkwzd_of_ids[] = {
	{ .compatible = "xlnx,clk-wizard-5.1" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, zynq_clkwzd_of_ids);

static struct platform_driver zynq_clkwzd_driver = {
	.probe = zynq_clkwzd_probe,
	.remove = zynq_clkwzd_remove,
	.driver = {
		.name = "zynq-clk-wizard",
		.of_match_table = zynq_clkwzd_of_ids,
	},
};
module_platform_driver(zynq_clkwzd_driver);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@bootlin.com>");
MODULE_DESCRIPTION("Xilinx Clocking Wizard driver");
MODULE_LICENSE("GPL");
