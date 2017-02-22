/*
 * GPIO driver for CSP System Registers
 *
 * Copyright (C) 2016 Jan Kotas <jank@cadence.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>


// ID register field definitions
#define CSP_ID_FLOW_MASK                    0xF
#define CSP_ID_FLOW_SHIFT                   0
#define CSP_ID_OUTPUT_MASK                  0x1
#define CSP_ID_OUTPUT_SHIFT                 4
#define CSP_ID_INPUT_MASK                   0x1
#define CSP_ID_INPUT_SHIFT                  5
#define CSP_ID_DUAL_SYSTEM_MASK             0x1
#define CSP_ID_DUAL_SYSTEM_SHIFT            6
#define CSP_ID_DUAL_SECOND_MASK             0x1
#define CSP_ID_DUAL_SECOND_SHIFT            7
#define CSP_ID_AUTOMATIC_TEST_MASK          0x1
#define CSP_ID_AUTOMATIC_TEST_SHIFT         8
#define CSP_ID_GUI_MASK                     0x1
#define CSP_ID_GUI_SHIFT                    9
#define CSP_ID_CONFIG_MASK                  0xFF
#define CSP_ID_CONFIG_SHIFT                 16
#define CSP_ID_MINOR_VERSION_MASK           0xF
#define CSP_ID_MINOR_VERSION_SHIFT          24
#define CSP_ID_MAJOR_VERSION_MASK           0xF
#define CSP_ID_MAJOR_VERSION_SHIFT          28

#define CSP_ID_GET_MASK(field)              CSP_ID_ ## field ## _MASK
#define CSP_ID_GET_SHIFT(field)             CSP_ID_ ## field ## _SHIFT
#define CSP_ID_READ_FIELD(field, source)    \
	(((source) >> CSP_ID_GET_SHIFT(field)) & CSP_ID_GET_MASK(field))


struct csp_gpio_regs {
	u32 id;
	u32 cpu_freq;
	u32 status;
	u32 run_stall;
	u32 software_reset;
	u32 core1_reset;
	u32 led_status;
	u32 proc_interrupt;
	u32 led_status_en;
	u32 scratch_led;
	u32 scratch_reg_3;
	u32 scratch_reg_4;
	u32 scratch_reg_5;
	u32 scratch_reg_6;
	u32 set_interrupt;
	u32 clr_interrupt;
	u32 dip_switches;
};

enum csp_gpio_types {
	CG_LED0, CG_LED1, CG_LED2, CG_LED3,
	CG_LED4, CG_LED5, CG_LED6, CG_LED7,
	CG_SWITCH0, CG_SWITCH1, CG_SWITCH2, CG_SWITCH3,
	CG_SWITCH4, CG_SWITCH5, CG_SWITCH6, CG_SWITCH7,
	CG_SWITCH8, CG_SWITCH9,

	CG_LAST
};

#define CSP_GPIO_COUNT	  CG_LAST


struct csp_gpio_chip {
	struct gpio_chip gpio;
	struct csp_gpio_regs __iomem *base;
	spinlock_t lock;
	bool enabled;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_root;
	/* Read only ID fields */
	struct {
		u8 major_version;
		u8 minor_version;
		bool automatic_test;
		bool gui;
		bool input;
		bool output;
		bool dual_system;
		bool dual_second;
	} id_reg;
#endif
};

static inline struct csp_gpio_chip *to_csp_gpio(struct gpio_chip *chip)
{
	return container_of(chip, struct csp_gpio_chip, gpio);
}

static inline void csp_gpio_enable(struct csp_gpio_chip *gc)
{
	if (!gc->enabled) {
		writel(1UL, &gc->base->led_status_en);
		gc->enabled = 1;
	}
}

static void csp_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	unsigned long flags;
	u32 reg;
	struct csp_gpio_chip *gc = to_csp_gpio(chip);

	spin_lock_irqsave(&gc->lock, flags);

	switch (offset) {
	case CG_LED0 ... CG_LED7:
		reg = readl(&gc->base->led_status);
		if (value)
			reg |= BIT(offset - CG_LED0);
		else
			reg &= ~BIT(offset - CG_LED0);
		writel(reg, &gc->base->led_status);
		break;
	case CG_SWITCH0 ... CG_SWITCH9:
		break;
	default:
		break;
	}

	csp_gpio_enable(gc);

	spin_unlock_irqrestore(&gc->lock, flags);
}

static int csp_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	u32 reg;
	struct csp_gpio_chip *gc = to_csp_gpio(chip);

	switch (offset) {
	case CG_LED0 ... CG_LED7:
		reg = readl(&gc->base->led_status);
		return ((reg >> (offset - CG_LED0)) & 1UL);
	case CG_SWITCH0 ... CG_SWITCH9:
		reg = readl(&gc->base->dip_switches);
		return ((reg >> (offset - CG_SWITCH0)) & 1UL);

	default:
		return -EINVAL;
	}
}

static int csp_gpio_get_direction(struct gpio_chip *chip, unsigned offset)
{
	switch (offset) {
	case CG_LED0 ... CG_LED7:
		return GPIOF_DIR_OUT;
	case CG_SWITCH0 ... CG_SWITCH9:
		return GPIOF_DIR_IN;
	default:
		return -EINVAL;
	}
}

static int csp_gpio_direction_input(struct gpio_chip *chip,
		unsigned offset)
{
	switch (offset) {
	case CG_LED0 ... CG_LED7:
		return -EIO;
	case CG_SWITCH0 ... CG_SWITCH9:
		return 0;
	default:
		return -EINVAL;
	}
}

static int csp_gpio_direction_output(struct gpio_chip *chip,
		unsigned offset, int value)
{
	switch (offset) {
	case CG_LED0 ... CG_LED7:
		csp_gpio_set(chip, offset, value);
		return 0;
	case CG_SWITCH0 ... CG_SWITCH9:
		return -EIO;

	default:
		return -EINVAL;
	}
}


static struct gpio_chip csp_chip = {
	.label = "CSP Sysregs",
	.direction_input = csp_gpio_direction_input,
	.direction_output = csp_gpio_direction_output,
	.get_direction = csp_gpio_get_direction,
	.set = csp_gpio_set,
	.get = csp_gpio_get,
	.ngpio = CSP_GPIO_COUNT,
	.base = -1,
	.owner = THIS_MODULE,
};

#ifdef CONFIG_DEBUG_FS

static ssize_t csp_debugfs_status_write(
	struct file *file, const char __user *user_buf,
	size_t count, loff_t *ppos)
{
	char buf[17];
	int ret;
	unsigned int value = 0;
	struct csp_gpio_chip *gc = file->private_data;

	if (count > (sizeof(buf) - 1))
		return count;

	ret = copy_from_user(buf, user_buf, count);
	if (ret)
		return -EFAULT;

	buf[count] = '\0';
	ret = kstrtouint(buf, 0, &value);
	if (ret)
		return ret;

	writel(value, &gc->base->led_status);
	csp_gpio_enable(gc);

	return count;
}

static ssize_t csp_debugfs_status_read(struct file *file, char __user *user_buf,
	size_t count, loff_t *ppos)
{
	char buf[16];
	int i;
	u32 status;
	struct csp_gpio_chip *gc = file->private_data;

	status = readl(&gc->base->led_status);
	i = scnprintf(buf, sizeof(buf), "0x%08X\n", status);

	return simple_read_from_buffer(user_buf, count, ppos, buf, i);
}

static const struct file_operations csp_debugfs_status_ops = {
	.owner = THIS_MODULE,
	.read = csp_debugfs_status_read,
	.write = csp_debugfs_status_write,
	.open = simple_open,
	.llseek = generic_file_llseek,
};


static ssize_t csp_debugfs_flow_read(struct file *file, char __user *user_buf,
	size_t count, loff_t *ppos)
{
	char buf[16];
	int i;
	u32 flow;
	const char * const names[] = {
		"FPGA",
		"PROTIUM",
		"ICE",
		"IXCOM",
		"SIM",
		"VSP",
	};
	struct csp_gpio_chip *gc = file->private_data;

	flow = CSP_ID_READ_FIELD(FLOW, readl(&gc->base->id));
	i = scnprintf(buf, sizeof(buf), "%d %s\n", flow,
		flow < ARRAY_SIZE(names) ? names[flow] : "UNKNOWN");

	return simple_read_from_buffer(user_buf, count, ppos, buf, i);
}

static const struct file_operations csp_debugfs_flow_ops = {
	.owner = THIS_MODULE,
	.read = csp_debugfs_flow_read,
	.open = simple_open,
	.llseek = generic_file_llseek,
};


static ssize_t csp_debugfs_config_read(struct file *file, char __user *user_buf,
	size_t count, loff_t *ppos)
{
	char buf[6];
	int i;
	u32 config;

	struct csp_gpio_chip *gc = file->private_data;

	config = CSP_ID_READ_FIELD(CONFIG, readl(&gc->base->id));
	i = scnprintf(buf, sizeof(buf), "0x%02X\n", config);

	return simple_read_from_buffer(user_buf, count, ppos, buf, i);
}

static const struct file_operations csp_debugfs_config_ops = {
	.owner = THIS_MODULE,
	.read = csp_debugfs_config_read,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

static void csp_debugfs_init(struct csp_gpio_chip *gc)
{
	u32 id_value = 0;

	gc->debugfs_root = debugfs_create_dir("csp-sysregs", NULL);
	if (!gc->debugfs_root) {
		pr_err("failed to create debugfs directory");
		return;
	}

	id_value = readl(&gc->base->id);

	gc->id_reg.automatic_test = CSP_ID_READ_FIELD(AUTOMATIC_TEST, id_value);
	gc->id_reg.gui = CSP_ID_READ_FIELD(GUI, id_value);
	gc->id_reg.input = CSP_ID_READ_FIELD(INPUT, id_value);
	gc->id_reg.output = CSP_ID_READ_FIELD(OUTPUT, id_value);
	gc->id_reg.dual_system = CSP_ID_READ_FIELD(DUAL_SYSTEM, id_value);
	gc->id_reg.dual_second = CSP_ID_READ_FIELD(DUAL_SECOND, id_value);

	gc->id_reg.major_version = CSP_ID_READ_FIELD(MAJOR_VERSION, id_value);
	gc->id_reg.minor_version = CSP_ID_READ_FIELD(MINOR_VERSION, id_value);

	debugfs_create_bool("automatic_test", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.automatic_test);
	debugfs_create_bool("gui", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.gui);
	debugfs_create_bool("input", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.input);
	debugfs_create_bool("output", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.output);
	debugfs_create_bool("dual_system", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.dual_system);
	debugfs_create_bool("dual_second", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.dual_second);

	debugfs_create_u8("major_version", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.major_version);
	debugfs_create_u8("minor_version", S_IRUGO, gc->debugfs_root,
		&gc->id_reg.minor_version);

	debugfs_create_file("status", S_IRUGO | S_IWUSR, gc->debugfs_root,
		gc, &csp_debugfs_status_ops);
	debugfs_create_file("flow", S_IRUGO, gc->debugfs_root,
		gc, &csp_debugfs_flow_ops);
	debugfs_create_file("config", S_IRUGO, gc->debugfs_root,
		gc, &csp_debugfs_config_ops);
}

static void csp_debugfs_exit(struct csp_gpio_chip *gc)
{
	debugfs_remove_recursive(gc->debugfs_root);
}
#endif


static int csp_gpio_probe(struct platform_device *pdev)
{
	struct csp_gpio_chip *gc;
	struct resource *res;
	int ret;

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	gc->gpio = csp_chip;
	platform_set_drvdata(pdev, gc);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	gc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(gc->base))
		return PTR_ERR(gc->base);

	spin_lock_init(&gc->lock);

	gc->gpio.parent = &pdev->dev;

	ret = gpiochip_add(&gc->gpio);
	if (ret) {
		dev_err(&pdev->dev, "failed to add gpio chip\n");
		return ret;
	}

	gc->enabled = 0;

#ifdef CONFIG_DEBUG_FS
	csp_debugfs_init(gc);
#endif

	return 0;
}

static int csp_gpio_remove(struct platform_device *pdev)
{
	struct csp_gpio_chip *gc = platform_get_drvdata(pdev);

	writel(0, &gc->base->led_status_en);

#ifdef CONFIG_DEBUG_FS
	csp_debugfs_exit(gc);
#endif

	gpiochip_remove(&gc->gpio);

	return 0;
}

static const struct of_device_id csp_gpio_match[] = {
	{ .compatible = "cdns,csp-gpio" },
	{ .compatible = "cdns,csp-sysregs" },
	{ }
};
MODULE_DEVICE_TABLE(of, csp_gpio_match);

static struct platform_driver csp_gpio_driver = {
	.probe = csp_gpio_probe,
	.remove = csp_gpio_remove,
	.driver = {
		.name = "csp-gpio",
		.owner = THIS_MODULE,
		.of_match_table = csp_gpio_match,
	},
};


module_platform_driver(csp_gpio_driver);

MODULE_AUTHOR("Jan Kotas <jank@cadence.com>");
MODULE_DESCRIPTION("GPIO driver for CSP Sysregs");
MODULE_LICENSE("GPL v2");

