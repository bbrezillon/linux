#include <linux/configfs.h>
#include <linux/mtd/mtd.h>

struct mtdsim_dev;

struct mtdsim_ops {
	int (*init)(struct mtdsim_dev *dev);
	void (*cleanup)(struct mtdsim_dev *dev);
};

struct mtdsim_dev {
	struct config_item cfg;
	struct mtd_info mtd;
	const char *filename;
	const char *partitions;
	struct file *file;
	bool active;
	const struct mtdsim_ops *ops;
};

static inline struct mtdsim_dev *
config_item_to_mtdsim_dev(struct config_item *cfg)
{
	return container_of(cfg, struct mtdsim_dev, cfg);
}

extern struct configfs_attribute mtdsim_filename_attr;
extern struct configfs_attribute mtdsim_status_attr;
extern struct configfs_attribute mtdsim_action_attr;
extern struct configfs_attribute mtdsim_partitions_attr;
extern struct configfs_attribute mtdsim_writesize_attr;
extern struct configfs_attribute mtdsim_oobsize_attr;
extern struct configfs_attribute mtdsim_erasesize_attr;

#define MTDSIM_DEFAULT_ATTRS			\
	&mtdsim_filename_attr,			\
	&mtdsim_status_attr,			\
	&mtdsim_partitions_attr,		\
	&mtdsim_action_attr

#ifdef CONFIG_MTD_SIM_NAND
int __init mtdsim_nand_init(struct config_group *parent);
void __exit mtdsim_nand_exit(struct config_group *parent);
#else
static inline int __init mtdsim_nand_init(struct config_group *parent)
{
	return 0;
}

static inline void __exit mtdsim_nand_exit(struct config_group *parent)
{
}
#endif
