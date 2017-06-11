#include <linux/slab.h>

#include "core.h"

struct mtdsim_nand {
	struct mtdsim_dev base;
	void *buf;
	int bufsize;
};

static inline struct mtdsim_nand *
mtdsim_dev_to_nand(struct mtdsim_dev *dev)
{
	return container_of(dev, struct mtdsim_nand, base);
}

static struct configfs_attribute *mtdsim_nand_item_attrs[] = {
	MTDSIM_DEFAULT_ATTRS,
	&mtdsim_writesize_attr,
	&mtdsim_oobsize_attr,
	&mtdsim_erasesize_attr,
	NULL,
};

static int mtdsim_nand_dev_init(struct mtdsim_dev *dev)
{
	struct mtdsim_nand *nand = mtdsim_dev_to_nand(dev);

	/* TODO: check writesize/oobsize/erasesize consistency. */
	nand->buf = kzalloc(dev->mtd.writesize + dev->mtd.oobsize, GFP_KERNEL);
	if (!nand->buf)
		return -ENOMEM;

	return 0;
}

static void mtdsim_nand_dev_cleanup(struct mtdsim_dev *dev)
{
	struct mtdsim_nand *nand = mtdsim_dev_to_nand(dev);

	kfree(nand->buf);
}

static const struct mtdsim_ops mtdsim_nand_ops = {
	.init = mtdsim_nand_dev_init,
	.cleanup = mtdsim_nand_dev_cleanup,
};

static struct config_item_type mtdsim_nand_item_type = {
	.ct_owner = THIS_MODULE,
	.ct_attrs = mtdsim_nand_item_attrs,
};

static struct config_item *mtdsim_nand_make_item(struct config_group *group,
						 const char *name)
{
	struct mtdsim_nand *nand;

	nand = kzalloc(sizeof(*nand), GFP_KERNEL);
	if (!nand)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(&nand->base.cfg, name,
				   &mtdsim_nand_item_type);

	return &nand->base.cfg;
}

static struct configfs_group_operations mtdsim_nand_group_ops = {
	.make_item = mtdsim_nand_make_item,
};

static struct config_item_type mtdsim_nand_group_type = {
	.ct_owner = THIS_MODULE,
	.ct_group_ops = &mtdsim_nand_group_ops,
};

static struct config_group *mtdsim_nand_group;

int __init mtdsim_nand_init(struct config_group *parent)
{
	mtdsim_nand_group = configfs_register_default_group(parent, "nand",
						&mtdsim_nand_group_type);

	return PTR_ERR_OR_ZERO(mtdsim_nand_group);
}

void __exit mtdsim_nand_exit(struct config_group *parent)
{
	configfs_unregister_default_group(mtdsim_nand_group);
}
