/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright 2018 - Bootlin
 *
 *  Authors:
 *	Boris Brezillon <boris.brezillon@free-electrons.com>
 */

#ifndef __LINUX_ECC_ENGINE_H
#define __LINUX_ECC_ENGINE_H

struct ecc_engine;

struct ecc_config {
	unsigned int blocksize;
	unsigned int strength;
};

struct ecc_ctx {
	struct ecc_engine *engine;
	unsigned int blocksize;
	unsigned int strength;
	unsigned int eccbytes;
	void *priv;
};

enum ecc_req_dir {
	ECC_REQ_IN,
	ECC_REQ_OUT,
};

struct ecc_req {
	struct ecc_ctx *ctx;
	struct {
		enum ecc_req_dir dir;
		struct {
			void *in;
			const void *out;
		} buf;
	} data;
	struct {
		void *buf;
		size_t len;
	} ecc;
	void *priv;
};

struct ecc_engine_ops {
	int (*enable)(struct ecc_engine *engine);
	int (*disable)(struct ecc_engine *engine);
	int (*ctx_create)(struct ecc_ctx *ctx, const struct ecc_config *cfg);
	void (*ctx_destroy)(struct ecc_ctx *ctx);
	int (*req_init)(struct ecc_req *req);
	void (*req_cleanup)(struct ecc_req *req);
	int (*req_start)(struct ecc_req *req);
	int (*req_stop)(struct ecc_req *req);
};

struct ecc_engine {
	struct device *parent;
	const struct ecc_engine_ops *ops;
};

#endif /* __LINUX_ECC_ENGINE_H */
