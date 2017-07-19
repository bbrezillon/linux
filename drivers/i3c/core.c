// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */

#include <linux/device.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/rwsem.h>
#include <linux/slab.h>

#include "internals.h"

static DEFINE_IDR(i3c_bus_idr);
static DEFINE_MUTEX(i3c_core_lock);

/**
 * i3c_bus_maintenance_lock - Lock the bus for a maintenance operation
 * @bus: I3C bus to take the lock on
 *
 * This function takes the bus lock so that no other operations can occur on
 * the bus. This is needed for all kind of bus maintenance operation, like
 * - enabling/disabling slave events
 * - re-triggering DAA
 * - changing the dynamic address of a device
 * - relinquishing mastership
 * - ...
 *
 * The reason for this kind of locking is that we don't want drivers and core
 * logic to rely on I3C device information that could be changed behind their
 * back.
 */
void i3c_bus_maintenance_lock(struct i3c_bus *bus)
{
	down_write(&bus->lock);
}
EXPORT_SYMBOL_GPL(i3c_bus_maintenance_lock);

/**
 * i3c_bus_maintenance_unlock - Release the bus lock after a maintenance
 *			      operation
 * @bus: I3C bus to release the lock on
 *
 * Should be called when the bus maintenance operation is done. See
 * i3c_bus_maintenance_lock() for more details on what these maintenance
 * operations are.
 */
void i3c_bus_maintenance_unlock(struct i3c_bus *bus)
{
	up_write(&bus->lock);
}
EXPORT_SYMBOL_GPL(i3c_bus_maintenance_unlock);

/**
 * i3c_bus_normaluse_lock - Lock the bus for a normal operation
 * @bus: I3C bus to take the lock on
 *
 * This function takes the bus lock for any operation that is not a maintenance
 * operation (see i3c_bus_maintenance_lock() for a non-exhaustive list of
 * maintenance operations). Basically all communications with I3C devices are
 * normal operations (HDR, SDR transfers or CCC commands that do not change bus
 * state or I3C dynamic address).
 *
 * Note that this lock is not guaranteeing serialization of normal operations.
 * In other words, transfer requests passed to the I3C master can be submitted
 * in parallel and I3C master drivers have to use their own locking to make
 * sure two different communications are not inter-mixed, or access to the
 * output/input queue is not done while the engine is busy.
 */
void i3c_bus_normaluse_lock(struct i3c_bus *bus)
{
	down_read(&bus->lock);
}
EXPORT_SYMBOL_GPL(i3c_bus_normaluse_lock);

/**
 * i3c_bus_normaluse_unlock - Release the bus lock after a normal operation
 * @bus: I3C bus to release the lock on
 *
 * Should be called when a normal operation is done. See
 * i3c_bus_normaluse_lock() for more details on what these normal operations
 * are.
 */
void i3c_bus_normaluse_unlock(struct i3c_bus *bus)
{
	up_read(&bus->lock);
}
EXPORT_SYMBOL_GPL(i3c_bus_normaluse_unlock);

static ssize_t bcr_show(struct device *dev,
			struct device_attribute *da,
			char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cdev->bus);
	ret = sprintf(buf, "%x\n", i3cdev->desc->info.bcr);
	i3c_bus_normaluse_unlock(i3cdev->bus);

	return ret;
}
static DEVICE_ATTR_RO(bcr);

static ssize_t dcr_show(struct device *dev,
			struct device_attribute *da,
			char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cdev->bus);
	ret = sprintf(buf, "%x\n", i3cdev->desc->info.dcr);
	i3c_bus_normaluse_unlock(i3cdev->bus);

	return ret;
}
static DEVICE_ATTR_RO(dcr);

static ssize_t pid_show(struct device *dev,
			struct device_attribute *da,
			char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cdev->bus);
	ret = sprintf(buf, "%llx\n", i3cdev->desc->info.pid);
	i3c_bus_normaluse_unlock(i3cdev->bus);

	return ret;
}
static DEVICE_ATTR_RO(pid);

static ssize_t dynamic_address_show(struct device *dev,
				    struct device_attribute *da,
				    char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cdev->bus);
	ret = sprintf(buf, "%02x\n", i3cdev->desc->info.dyn_addr);
	i3c_bus_normaluse_unlock(i3cdev->bus);

	return ret;
}
static DEVICE_ATTR_RO(dynamic_address);

static const char * const hdrcap_strings[] = {
	"hdr-ddr", "hdr-tsp", "hdr-tsl",
};

static ssize_t hdrcap_show(struct device *dev,
			   struct device_attribute *da,
			   char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	ssize_t offset = 0, ret;
	unsigned long caps;
	int mode;

	i3c_bus_normaluse_lock(i3cdev->bus);
	caps = i3cdev->desc->info.hdr_cap;
	for_each_set_bit(mode, &caps, 8) {
		if (mode >= ARRAY_SIZE(hdrcap_strings))
			break;

		if (!hdrcap_strings[mode])
			continue;

		ret = sprintf(buf + offset, offset ? " %s" : "%s",
			      hdrcap_strings[mode]);
		if (ret < 0)
			goto out;

		offset += ret;
	}

	ret = sprintf(buf + offset, "\n");
	if (ret < 0)
		goto out;

	ret = offset + ret;

out:
	i3c_bus_normaluse_unlock(i3cdev->bus);

	return ret;
}
static DEVICE_ATTR_RO(hdrcap);

static struct attribute *i3c_device_attrs[] = {
	&dev_attr_bcr.attr,
	&dev_attr_dcr.attr,
	&dev_attr_pid.attr,
	&dev_attr_dynamic_address.attr,
	&dev_attr_hdrcap.attr,
	NULL,
};
ATTRIBUTE_GROUPS(i3c_device);

static int i3c_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_device_info devinfo;
	u16 manuf, part, ext;

	i3c_device_get_info(i3cdev, &devinfo);
	manuf = I3C_PID_MANUF_ID(devinfo.pid);
	part = I3C_PID_PART_ID(devinfo.pid);
	ext = I3C_PID_EXTRA_INFO(devinfo.pid);

	if (I3C_PID_RND_LOWER_32BITS(devinfo.pid))
		return add_uevent_var(env, "MODALIAS=i3c:dcr%02Xmanuf%04X",
				      devinfo.dcr, manuf);

	return add_uevent_var(env,
			      "MODALIAS=i3c:dcr%02Xmanuf%04Xpart%04xext%04x",
			      devinfo.dcr, manuf, part, ext);
}

const struct device_type i3c_device_type = {
	.groups	= i3c_device_groups,
	.uevent = i3c_device_uevent,
};

const struct device_type i3c_master_type = {
	.groups	= i3c_device_groups,
};

static const struct i3c_device_id *
i3c_device_match_id(struct i3c_device *i3cdev,
		    const struct i3c_device_id *id_table)
{
	struct i3c_device_info devinfo;
	const struct i3c_device_id *id;

	i3c_device_get_info(i3cdev, &devinfo);

	/*
	 * The lower 32bits of the provisional ID is just filled with a random
	 * value, try to match using DCR info.
	 */
	if (!I3C_PID_RND_LOWER_32BITS(devinfo.pid)) {
		u16 manuf = I3C_PID_MANUF_ID(devinfo.pid);
		u16 part = I3C_PID_PART_ID(devinfo.pid);
		u16 ext_info = I3C_PID_EXTRA_INFO(devinfo.pid);

		/* First try to match by manufacturer/part ID. */
		for (id = id_table; id->match_flags != 0; id++) {
			if ((id->match_flags & I3C_MATCH_MANUF_AND_PART) !=
			    I3C_MATCH_MANUF_AND_PART)
				continue;

			if (manuf != id->manuf_id || part != id->part_id)
				continue;

			if ((id->match_flags & I3C_MATCH_EXTRA_INFO) &&
			    ext_info != id->extra_info)
				continue;

			return id;
		}
	}

	/* Fallback to DCR match. */
	for (id = id_table; id->match_flags != 0; id++) {
		if ((id->match_flags & I3C_MATCH_DCR) &&
		    id->dcr == devinfo.dcr)
			return id;
	}

	return NULL;
}

static int i3c_device_match(struct device *dev, struct device_driver *drv)
{
	struct i3c_device *i3cdev;
	struct i3c_driver *i3cdrv;

	if (dev->type != &i3c_device_type)
		return 0;

	i3cdev = dev_to_i3cdev(dev);
	i3cdrv = drv_to_i3cdrv(drv);
	if (i3c_device_match_id(i3cdev, i3cdrv->id_table))
		return 1;

	return 0;
}

static int i3c_device_probe(struct device *dev)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_driver *driver = drv_to_i3cdrv(dev->driver);

	return driver->probe(i3cdev);
}

static int i3c_device_remove(struct device *dev)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_driver *driver = drv_to_i3cdrv(dev->driver);
	int ret;

	ret = driver->remove(i3cdev);
	if (ret)
		return ret;

	i3c_device_free_ibi(i3cdev);

	return ret;
}

struct bus_type i3c_bus_type = {
	.name = "i3c",
	.match = i3c_device_match,
	.probe = i3c_device_probe,
	.remove = i3c_device_remove,
};

enum i3c_addr_slot_status i3c_bus_get_addr_slot_status(struct i3c_bus *bus,
						       u16 addr)
{
	int status, bitpos = addr * 2;

	if (addr > I2C_MAX_ADDR)
		return I3C_ADDR_SLOT_RSVD;

	status = bus->addrslots[bitpos / BITS_PER_LONG];
	status >>= bitpos % BITS_PER_LONG;

	return status & I3C_ADDR_SLOT_STATUS_MASK;
}

void i3c_bus_set_addr_slot_status(struct i3c_bus *bus, u16 addr,
				  enum i3c_addr_slot_status status)
{
	int bitpos = addr * 2;
	unsigned long *ptr;

	if (addr > I2C_MAX_ADDR)
		return;

	ptr = bus->addrslots + (bitpos / BITS_PER_LONG);
	*ptr &= ~(I3C_ADDR_SLOT_STATUS_MASK << (bitpos % BITS_PER_LONG));
	*ptr |= status << (bitpos % BITS_PER_LONG);
}

bool i3c_bus_dev_addr_is_avail(struct i3c_bus *bus, u8 addr)
{
	enum i3c_addr_slot_status status;

	status = i3c_bus_get_addr_slot_status(bus, addr);

	return status == I3C_ADDR_SLOT_FREE;
}

int i3c_bus_get_free_addr(struct i3c_bus *bus, u8 start_addr)
{
	enum i3c_addr_slot_status status;
	u8 addr;

	for (addr = start_addr; addr < I3C_MAX_ADDR; addr++) {
		status = i3c_bus_get_addr_slot_status(bus, addr);
		if (status == I3C_ADDR_SLOT_FREE)
			return addr;
	}

	return -ENOMEM;
}

static void i3c_bus_init_addrslots(struct i3c_bus *bus)
{
	int i;

	/* Addresses 0 to 7 are reserved. */
	for (i = 0; i < 8; i++)
		i3c_bus_set_addr_slot_status(bus, i, I3C_ADDR_SLOT_RSVD);

	/*
	 * Reserve broadcast address and all addresses that might collide
	 * with the broadcast address when facing a single bit error.
	 */
	i3c_bus_set_addr_slot_status(bus, I3C_BROADCAST_ADDR,
				     I3C_ADDR_SLOT_RSVD);
	for (i = 0; i < 7; i++)
		i3c_bus_set_addr_slot_status(bus, I3C_BROADCAST_ADDR ^ BIT(i),
					     I3C_ADDR_SLOT_RSVD);
}

static const char * const i3c_bus_mode_strings[] = {
	[I3C_BUS_MODE_PURE] = "pure",
	[I3C_BUS_MODE_MIXED_FAST] = "mixed-fast",
	[I3C_BUS_MODE_MIXED_SLOW] = "mixed-slow",
};

static ssize_t mode_show(struct device *dev,
			 struct device_attribute *da,
			 char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cbus);
	if (i3cbus->mode < 0 ||
	    i3cbus->mode > ARRAY_SIZE(i3c_bus_mode_strings) ||
	    !i3c_bus_mode_strings[i3cbus->mode])
		ret = sprintf(buf, "unknown\n");
	else
		ret = sprintf(buf, "%s\n", i3c_bus_mode_strings[i3cbus->mode]);
	i3c_bus_normaluse_unlock(i3cbus);

	return ret;
}
static DEVICE_ATTR_RO(mode);

static ssize_t current_master_show(struct device *dev,
				   struct device_attribute *da,
				   char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cbus);
	ret = sprintf(buf, "%d-%llx\n", i3cbus->id,
		      i3cbus->cur_master->info.pid);
	i3c_bus_normaluse_unlock(i3cbus);

	return ret;
}
static DEVICE_ATTR_RO(current_master);

static ssize_t i3c_scl_frequency_show(struct device *dev,
				      struct device_attribute *da,
				      char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cbus);
	ret = sprintf(buf, "%ld\n", i3cbus->scl_rate.i3c);
	i3c_bus_normaluse_unlock(i3cbus);

	return ret;
}
static DEVICE_ATTR_RO(i3c_scl_frequency);

static ssize_t i2c_scl_frequency_show(struct device *dev,
				      struct device_attribute *da,
				      char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_normaluse_lock(i3cbus);
	ret = sprintf(buf, "%ld\n", i3cbus->scl_rate.i2c);
	i3c_bus_normaluse_unlock(i3cbus);

	return ret;
}
static DEVICE_ATTR_RO(i2c_scl_frequency);

static struct attribute *i3c_busdev_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_current_master.attr,
	&dev_attr_i3c_scl_frequency.attr,
	&dev_attr_i2c_scl_frequency.attr,
	NULL,
};
ATTRIBUTE_GROUPS(i3c_busdev);

static void i3c_busdev_release(struct device *dev)
{
	struct i3c_bus *bus = container_of(dev, struct i3c_bus, dev);

	WARN_ON(!list_empty(&bus->devs.i2c) || !list_empty(&bus->devs.i3c));

	mutex_lock(&i3c_core_lock);
	idr_remove(&i3c_bus_idr, bus->id);
	mutex_unlock(&i3c_core_lock);

	of_node_put(bus->dev.of_node);
	kfree(bus);
}

static const struct device_type i3c_busdev_type = {
	.groups	= i3c_busdev_groups,
};

void i3c_bus_unref(struct i3c_bus *bus)
{
	put_device(&bus->dev);
}

struct i3c_bus *i3c_bus_create(struct device *parent)
{
	struct i3c_bus *i3cbus;
	int ret;

	i3cbus = kzalloc(sizeof(*i3cbus), GFP_KERNEL);
	if (!i3cbus)
		return ERR_PTR(-ENOMEM);

	init_rwsem(&i3cbus->lock);
	INIT_LIST_HEAD(&i3cbus->devs.i2c);
	INIT_LIST_HEAD(&i3cbus->devs.i3c);
	i3c_bus_init_addrslots(i3cbus);
	i3cbus->mode = I3C_BUS_MODE_PURE;
	i3cbus->dev.parent = parent;
	i3cbus->dev.of_node = of_node_get(parent->of_node);
	i3cbus->dev.bus = &i3c_bus_type;
	i3cbus->dev.type = &i3c_busdev_type;
	i3cbus->dev.release = i3c_busdev_release;

	mutex_lock(&i3c_core_lock);
	ret = idr_alloc(&i3c_bus_idr, i3cbus, 0, 0, GFP_KERNEL);
	mutex_unlock(&i3c_core_lock);
	if (ret < 0)
		goto err_free_bus;

	i3cbus->id = ret;
	device_initialize(&i3cbus->dev);

	return i3cbus;

err_free_bus:
	kfree(i3cbus);

	return ERR_PTR(ret);
}

void i3c_bus_unregister(struct i3c_bus *bus)
{
	device_unregister(&bus->dev);
}

int i3c_bus_register(struct i3c_bus *i3cbus)
{
	struct i2c_dev_desc *desc;

	i3c_bus_for_each_i2cdev(i3cbus, desc) {
		switch (desc->boardinfo->lvr & I3C_LVR_I2C_INDEX_MASK) {
		case I3C_LVR_I2C_INDEX(0):
			if (i3cbus->mode < I3C_BUS_MODE_MIXED_FAST)
				i3cbus->mode = I3C_BUS_MODE_MIXED_FAST;
			break;

		case I3C_LVR_I2C_INDEX(1):
		case I3C_LVR_I2C_INDEX(2):
			if (i3cbus->mode < I3C_BUS_MODE_MIXED_SLOW)
				i3cbus->mode = I3C_BUS_MODE_MIXED_SLOW;
			break;

		default:
			return -EINVAL;
		}
	}

	if (!i3cbus->scl_rate.i3c)
		i3cbus->scl_rate.i3c = I3C_BUS_TYP_I3C_SCL_RATE;

	if (!i3cbus->scl_rate.i2c) {
		if (i3cbus->mode == I3C_BUS_MODE_MIXED_SLOW)
			i3cbus->scl_rate.i2c = I3C_BUS_I2C_FM_SCL_RATE;
		else
			i3cbus->scl_rate.i2c = I3C_BUS_I2C_FM_PLUS_SCL_RATE;
	}

	/*
	 * I3C/I2C frequency may have been overridden, check that user-provided
	 * values are not exceeding max possible frequency.
	 */
	if (i3cbus->scl_rate.i3c > I3C_BUS_MAX_I3C_SCL_RATE ||
	    i3cbus->scl_rate.i2c > I3C_BUS_I2C_FM_PLUS_SCL_RATE) {
		return -EINVAL;
	}

	dev_set_name(&i3cbus->dev, "i3c-%d", i3cbus->id);

	return device_add(&i3cbus->dev);
}

static int __init i3c_init(void)
{
	return bus_register(&i3c_bus_type);
}
subsys_initcall(i3c_init);

static void __exit i3c_exit(void)
{
	idr_destroy(&i3c_bus_idr);
	bus_unregister(&i3c_bus_type);
}
module_exit(i3c_exit);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@bootlin.com>");
MODULE_DESCRIPTION("I3C core");
MODULE_LICENSE("GPL v2");
