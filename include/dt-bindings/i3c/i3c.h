/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2017 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */

#ifndef _DT_BINDINGS_I3C_I3C_H
#define _DT_BINDINGS_I3C_I3C_H

#define IS_I2C_DEV		0x80000000

#define I2C_DEV(addr, lvr)					\
	(addr) (IS_I2C_DEV | (lvr)) 0x0

#define I3C_PID(manufid, partid, instid, extrainfo)		\
	((manufid) << 1)					\
	(((partid) << 16) | ((instid) << 12) | (extrainfo))

#define I3C_DEV_WITH_STATIC_ADDR(addr, manufid, partid,		\
				 instid, extrainfo)		\
	(addr) I3C_PID(manufid, partid, instid, extrainfo)

#define I3C_DEV(manufid, partid, instid, extrainfo)		\
	I3C_DEV_WITH_STATIC_ADDR(0x0, manufid, partid,		\
				 instid, extrainfo)

#endif
