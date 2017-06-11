#include <linux/module.h>
#include <linux/slab.h>

#include "core.h"

static ssize_t mtdsim_filename_show(struct config_item *cfg, char *buf)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);

	if (!dev->filename)
		return 0;

	strcpy(buf, dev->filename);

	return strlen(dev->filename) + 1;
}

static ssize_t mtdsim_filename_store(struct config_item *cfg, const char *buf,
				     size_t size)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);
	const char *filename;

	if (dev->active)
		return -EBUSY;

	filename = kstrndup(buf, size, GFP_KERNEL);
	if (!filename)
		return -ENOMEM;

	kfree(dev->filename);
	dev->filename = filename;

	return size;
}

struct configfs_attribute mtdsim_filename_attr = {
	.ca_name = "filename",
	.ca_mode = S_IRUGO | S_IWUSR,
	.ca_owner = THIS_MODULE,
	.show = mtdsim_filename_show,
	.store = mtdsim_filename_store,
};

static ssize_t mtdsim_status_show(struct config_item *cfg, char *buf)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);

	if (dev->active)
		strcpy(buf, "active\n");
	else
		strcpy(buf, "inactive\n");

	return strlen(buf);
}

struct configfs_attribute mtdsim_status_attr = {
	.ca_name = "status",
	.ca_mode = S_IRUGO,
	.ca_owner = THIS_MODULE,
	.show = mtdsim_status_show,
};

static int mtdsim_register(struct mtdsim_dev *dev)
{
	struct mtd_partition *parts = NULL;
	int nparts = 0, ret;

	if (dev->active)
		return 0;

	if (dev->partitions) {
		ret = mtdpart_parse_cmdline_fmt(dev->partitions, &parts,
						&nparts);
		if (ret) {
			pr_err("Failed to parse partitions def\n");
			return ret;
		}
	}

	dev->file = filp_open(dev->filename, O_CREAT | O_RDWR | O_LARGEFILE,
			      S_IRUSR | S_IWUSR);
	if (IS_ERR(dev->file)) {
		pr_err("Failed to open storage file\n");
		ret = PTR_ERR(dev->file);
		goto err_free_parts;
	}

	ret = mtd_device_register(&dev->mtd, parts, nparts);
	if (ret) {
		pr_err("Failed to register MTD sim device\n");
		goto err_close_file;
	}

	dev->active = true;
	kfree(parts);

	return 0;

err_close_file:
	filp_close(dev->file, NULL);

err_free_parts:
	kfree(parts);

	return ret;
}

static int mtdsim_unregister(struct mtdsim_dev *dev)
{
	int ret;

	if (!dev->active)
		return 0;

	ret = mtd_device_unregister(&dev->mtd);
	if (ret)
		return ret;

	vfs_fsync(dev->file, 1);
	filp_close(dev->file, NULL);

	dev->active = false;

	return 0;
}

static ssize_t mtdsim_action_store(struct config_item *cfg, const char *buf,
				   size_t size)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);
	int ret = -EINVAL;

	if (!strcmp(buf, "register"))
		ret = mtdsim_register(dev);
	else if (!strcmp(buf, "unregister"))
		ret = mtdsim_unregister(dev);

	if (ret)
		return ret;

	return size;
}

struct configfs_attribute mtdsim_action_attr = {
	.ca_name = "action",
	.ca_mode = S_IWUSR,
	.ca_owner = THIS_MODULE,
	.store = mtdsim_action_store,
};

static ssize_t mtdsim_writesize_show(struct config_item *cfg, char *buf)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);

	return sprintf(buf, "%d\n", dev->mtd.writesize);
}

static ssize_t mtdsim_writesize_store(struct config_item *cfg, const char *buf,
				      size_t size)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);
	unsigned long val;
	int ret;

	if (dev->active)
		return -EBUSY;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	dev->mtd.writesize = val;

	return size;
}

struct configfs_attribute mtdsim_writesize_attr = {
	.ca_name = "writesize",
	.ca_mode = S_IRUGO | S_IWUSR,
	.ca_owner = THIS_MODULE,
	.show = mtdsim_writesize_show,
	.store = mtdsim_writesize_store,
};

static ssize_t mtdsim_oobsize_show(struct config_item *cfg, char *buf)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);

	return sprintf(buf, "%d\n", dev->mtd.oobsize);
}

static ssize_t mtdsim_oobsize_store(struct config_item *cfg, const char *buf,
				    size_t size)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);
	unsigned long val;
	int ret;

	if (dev->active)
		return -EBUSY;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	dev->mtd.oobsize = val;

	return size;
}

struct configfs_attribute mtdsim_oobsize_attr = {
	.ca_name = "oobsize",
	.ca_mode = S_IRUGO | S_IWUSR,
	.ca_owner = THIS_MODULE,
	.show = mtdsim_oobsize_show,
	.store = mtdsim_oobsize_store,
};

static ssize_t mtdsim_erasesize_show(struct config_item *cfg, char *buf)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);

	return sprintf(buf, "%d\n", dev->mtd.erasesize);
}

static ssize_t mtdsim_erasesize_store(struct config_item *cfg, const char *buf,
				      size_t size)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	dev->mtd.erasesize = val;

	return size;
}

struct configfs_attribute mtdsim_erasesize_attr = {
	.ca_name = "erasesize",
	.ca_mode = S_IRUGO | S_IWUSR,
	.ca_owner = THIS_MODULE,
	.show = mtdsim_erasesize_show,
	.store = mtdsim_erasesize_store,
};

static ssize_t mtdsim_partitions_show(struct config_item *cfg, char *buf)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);

	if (!dev->partitions)
		return 0;

	return snprintf(buf, PAGE_SIZE, "%s\n", dev->partitions);
}

static ssize_t mtdsim_partitions_store(struct config_item *cfg,
				       const char *buf, size_t size)
{
	struct mtdsim_dev *dev = config_item_to_mtdsim_dev(cfg);
	const char *parts;

	if (dev->active)
		return -EBUSY;

	parts = kstrndup(buf, size, GFP_KERNEL);
	if (!parts)
		return -ENOMEM;

	kfree(dev->partitions);
	dev->partitions = parts;

	return size;
}

struct configfs_attribute mtdsim_partitions_attr = {
	.ca_name = "partitions",
	.ca_mode = S_IRUGO | S_IWUSR,
	.ca_owner = THIS_MODULE,
	.show = mtdsim_partitions_show,
	.store = mtdsim_partitions_store,
};

static struct config_item_type mtdsim_root_group_type = {
	.ct_owner = THIS_MODULE,
};

static struct configfs_subsystem mtdsim_configfs_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "mtdsim",
			.ci_type = &mtdsim_root_group_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(mtdsim_configfs_subsys.su_mutex),
};

static int __init mtdsim_init(void)
{
	int ret;

	config_group_init(&mtdsim_configfs_subsys.su_group);

	ret = configfs_register_subsystem(&mtdsim_configfs_subsys);
	if (ret)
		return ret;

	ret = mtdsim_nand_init(&mtdsim_configfs_subsys.su_group);
	if (ret)
		goto err_unregister_subsys;

	return 0;

err_unregister_subsys:
	configfs_unregister_subsystem(&mtdsim_configfs_subsys);

	return ret;
}
module_init(mtdsim_init);

static void __exit mtdsim_exit(void)
{
	mtdsim_nand_exit(&mtdsim_configfs_subsys.su_group);
	configfs_unregister_subsystem(&mtdsim_configfs_subsys);
}
module_exit(mtdsim_exit);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@free-electrons.com>");
MODULE_DESCRIPTION("MTD simulator");
MODULE_LICENSE("GPL v2");
