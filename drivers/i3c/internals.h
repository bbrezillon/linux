/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */

#ifndef I3C_INTERNALS_H
#define I3C_INTERNALS_H

#include <linux/i3c/master.h>

extern struct bus_type i3c_bus_type;
extern const struct device_type i3c_master_type;
extern const struct device_type i3c_device_type;

void i3c_bus_unref(struct i3c_bus *bus);
struct i3c_bus *i3c_bus_create(struct device *parent);
void i3c_bus_unregister(struct i3c_bus *bus);
int i3c_bus_register(struct i3c_bus *i3cbus);
int i3c_bus_get_free_addr(struct i3c_bus *bus, u8 start_addr);
bool i3c_bus_dev_addr_is_avail(struct i3c_bus *bus, u8 addr);
void i3c_bus_set_addr_slot_status(struct i3c_bus *bus, u16 addr,
				  enum i3c_addr_slot_status status);
enum i3c_addr_slot_status i3c_bus_get_addr_slot_status(struct i3c_bus *bus,
						       u16 addr);

int i3c_dev_do_priv_xfers_locked(struct i3c_dev_desc *dev,
				 struct i3c_priv_xfer *xfers,
				 int nxfers);
int i3c_dev_disable_ibi_locked(struct i3c_dev_desc *dev);
int i3c_dev_enable_ibi_locked(struct i3c_dev_desc *dev);
int i3c_dev_request_ibi_locked(struct i3c_dev_desc *dev,
			       const struct i3c_ibi_setup *req);
void i3c_dev_free_ibi_locked(struct i3c_dev_desc *dev);
#endif /* I3C_INTERNAL_H */
