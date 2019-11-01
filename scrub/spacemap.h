// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_SPACEMAP_H_
#define XFS_SCRUB_SPACEMAP_H_

/*
 * Visit each space mapping in the filesystem.  Return true to continue
 * iteration or false to stop iterating and return to the caller.
 */
typedef bool (*xfs_fsmap_iter_fn)(struct scrub_ctx *ctx, const char *descr,
		struct fsmap *fsr, void *arg);

bool xfs_iterate_fsmap(struct scrub_ctx *ctx, const char *descr,
		struct fsmap *keys, xfs_fsmap_iter_fn fn, void *arg);
bool xfs_scan_all_spacemaps(struct scrub_ctx *ctx, xfs_fsmap_iter_fn fn,
		void *arg);

#endif /* XFS_SCRUB_SPACEMAP_H_ */
