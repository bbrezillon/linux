/*
 * Copyright (C) Cadence 2017
 *
 * Author: Boris Brezillon <boris.brezillon@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/delay.h>

static ssize_t gpo_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct i3c_device *i3cdev = container_of(dev, struct i3c_device, dev);
	u8 reg = 5, gpo[2] = {};
	int ret;
	struct i3c_priv_xfer xfers[] = {
		{
			.len = 1,
			.data.out = &reg,
		},
		{
			.rnw = true,
			.len = 1,
			.data.in = gpo,
		},
	};

	pr_info("%s:%i\n", __func__, __LINE__);
	ret = i3c_device_do_priv_xfers(i3cdev, xfers, ARRAY_SIZE(xfers));
	pr_info("%s:%i\n", __func__, __LINE__);
	if (ret)
		return ret;

	pr_info("%s:%i\n", __func__, __LINE__);
	return sprintf(buf, "%02x %02x\n", gpo[0], gpo[1]);
}

static ssize_t gpo_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct i3c_device *i3cdev = container_of(dev, struct i3c_device, dev);
	u8 reg = 5, gpo = 0;
	struct i3c_priv_xfer xfers[] = {
		{
			.len = 1,
			.data.out = &reg,
		},
		{
			.len = 1,
			.data.out = &gpo,
		}
	};
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	gpo = val;
	ret = i3c_device_do_priv_xfers(i3cdev, xfers, ARRAY_SIZE(xfers));
	if (ret)
		return ret;

	return count;
}

static const DEVICE_ATTR_RW(gpo);

static ssize_t ddr_msg_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct i3c_device *i3cdev = container_of(dev, struct i3c_device, dev);
	u16 data[4] = {};
	struct i3c_hdr_cmd hdrcmd = {
		.mode = I3C_HDR_DDR,
		.code = 0x80,
		.ndatawords = ARRAY_SIZE(data),
		.data.in = data,
	};
	int ret;

	ret = i3c_device_send_hdr_cmds(i3cdev, &hdrcmd, 1);
	if (ret)
		return ret;

	return sprintf(buf, "%04x %04x %04x %04x\n", data[0], data[1],
		       data[2], data[3]);
}

static ssize_t ddr_msg_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct i3c_device *i3cdev = container_of(dev, struct i3c_device, dev);
	u16 data[4] = {0xa, 0xb, 0xc, 0xd};
	struct i3c_hdr_cmd hdrcmd = {
		.mode = I3C_HDR_DDR,
		.code = 0x00,
		.ndatawords = ARRAY_SIZE(data),
		.data.in = data,
	};
	int ret;

	ret = i3c_device_send_hdr_cmds(i3cdev, &hdrcmd, 1);
	if (ret)
		return ret;

	return count;
}
static const DEVICE_ATTR_RW(ddr_msg);

static void ibi_handler(struct i3c_device *dev,
			const struct i3c_ibi_payload *payload)
{
	pr_info("%s:%i\n", __func__, __LINE__);
}

static int dummy_i3c_probe(struct i3c_device *dev)
{
	struct i3c_ibi_setup ibireq;
	struct i3c_device_info devinfo;
	int ret;

	pr_info("%s:%i\n", __func__, __LINE__);
	i3c_device_get_info(dev, &devinfo);

	if (I3C_PID_PART_ID(devinfo.pid) == 0x13) {
		pr_info("%s:%i\n", __func__, __LINE__);
		if (devinfo.bcr & I3C_BCR_HDR_CAP)
			device_create_file(&dev->dev, &dev_attr_ddr_msg);

		pr_info("%s:%i\n", __func__, __LINE__);
		return device_create_file(&dev->dev, &dev_attr_gpo);
	}

	pr_info("%s:%i part = %08x\n", __func__, __LINE__, (u32)I3C_PID_PART_ID(devinfo.pid));
	ibireq.handler = ibi_handler;
	ibireq.max_payload_len = 2;
	ibireq.num_slots = 10;
	ret = i3c_device_request_ibi(dev, &ibireq);
	if (ret)
		return ret;

	pr_info("%s:%i\n", __func__, __LINE__);
	ret = i3c_device_enable_ibi(dev);
	pr_info("%s:%i ret = %d\n", __func__, __LINE__, ret);
	return ret;
}

static int dummy_i3c_remove(struct i3c_device *dev)
{
	pr_info("%s:%i\n", __func__, __LINE__);
	device_remove_file(&dev->dev, &dev_attr_ddr_msg);
	pr_info("%s:%i\n", __func__, __LINE__);
	device_remove_file(&dev->dev, &dev_attr_gpo);
	pr_info("%s:%i\n", __func__, __LINE__);
	i3c_device_disable_ibi(dev);
	pr_info("%s:%i\n", __func__, __LINE__);
	i3c_device_free_ibi(dev);
	pr_info("%s:%i\n", __func__, __LINE__);
	return 0;
}

static const struct i3c_device_id dummy_i3c_ids[] = {
	I3C_DEVICE(0x1c9, 0x13, NULL),
	I3C_DEVICE(0x1c9, 0x14, NULL),
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i3c, dummy_i3c_ids);

static struct i3c_driver dummy_i3c_drv = {
	.driver = {
		.name = "cdns-dummy-i3c",
	},
	.id_table = dummy_i3c_ids,
	.probe = dummy_i3c_probe,
	.remove = dummy_i3c_remove,
};
module_i3c_driver(dummy_i3c_drv);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@free-electrons.com>");
MODULE_DESCRIPTION("I3C Test driver");
MODULE_LICENSE("GPL v2");
