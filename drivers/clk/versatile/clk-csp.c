/*
 * CSP Sysregs Clock driver
 *
 * Copyright (C) 2016 Cadence Design Systems, Inc.
 * All rights reserved worldwide.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/regmap.h>

#define CSP_ADDR_CPU_FREQ_REG	4

static void __init of_csp_clk_setup(struct device_node *node)
{
	struct clk *clk;
	const char *clk_name = node->name;
	void __iomem *base;
	u32 clk_freq;

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("csp-clock: failed to map address\n");
		return;
	}

	clk_freq = readl(base + CSP_ADDR_CPU_FREQ_REG);

	iounmap(base);

	pr_info("csp-clock: found %s @ %u Hz\n", clk_name, clk_freq);

	clk = clk_register_fixed_rate(NULL, clk_name, NULL, 0, clk_freq);
	if (IS_ERR(clk)) {
		pr_err("csp-clock: failed to register fixed rate clock\n");
		return;
	}

	of_clk_add_provider(node, of_clk_src_simple_get, clk);
}

CLK_OF_DECLARE(csp_clk, "cdns,csp-clock", of_csp_clk_setup);

