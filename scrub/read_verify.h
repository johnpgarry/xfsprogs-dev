// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_READ_VERIFY_H_
#define XFS_SCRUB_READ_VERIFY_H_

struct scrub_ctx;
struct read_verify_pool;

/* Function called when an IO error happens. */
typedef void (*read_verify_ioerr_fn_t)(struct scrub_ctx *ctx,
		struct disk *disk, uint64_t start, uint64_t length,
		int error, void *arg);

struct read_verify_pool *read_verify_pool_init(struct scrub_ctx *ctx,
		size_t miniosz, read_verify_ioerr_fn_t ioerr_fn,
		unsigned int nproc);
void read_verify_pool_flush(struct read_verify_pool *rvp);
void read_verify_pool_destroy(struct read_verify_pool *rvp);

struct read_verify {
	void			*io_end_arg;
	struct disk		*io_disk;
	uint64_t		io_start;	/* bytes */
	uint64_t		io_length;	/* bytes */
};

bool read_verify_schedule_io(struct read_verify_pool *rvp,
		struct read_verify *rv, struct disk *disk, uint64_t start,
		uint64_t length, void *end_arg);
bool read_verify_force_io(struct read_verify_pool *rvp, struct read_verify *rv);
uint64_t read_verify_bytes(struct read_verify_pool *rvp);

#endif /* XFS_SCRUB_READ_VERIFY_H_ */
