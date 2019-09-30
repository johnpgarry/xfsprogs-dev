// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_INODES_H_
#define XFS_SCRUB_INODES_H_

typedef int (*xfs_inode_iter_fn)(struct scrub_ctx *ctx,
		struct xfs_handle *handle, struct xfs_bulkstat *bs, void *arg);

#define XFS_ITERATE_INODES_ABORT	(-1)
bool xfs_scan_all_inodes(struct scrub_ctx *ctx, xfs_inode_iter_fn fn,
		void *arg);

int xfs_open_handle(struct xfs_handle *handle);

#endif /* XFS_SCRUB_INODES_H_ */
