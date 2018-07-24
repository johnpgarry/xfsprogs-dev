// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_FILEMAP_H_
#define XFS_SCRUB_FILEMAP_H_

/* inode fork block mapping */
struct xfs_bmap {
	uint64_t	bm_offset;	/* file offset of segment in bytes */
	uint64_t	bm_physical;	/* physical starting byte  */
	uint64_t	bm_length;	/* length of segment, bytes */
	uint32_t	bm_flags;	/* output flags */
};

typedef bool (*xfs_bmap_iter_fn)(struct scrub_ctx *ctx, const char *descr,
		int fd, int whichfork, struct fsxattr *fsx,
		struct xfs_bmap *bmap, void *arg);

bool xfs_iterate_filemaps(struct scrub_ctx *ctx, const char *descr, int fd,
		int whichfork, struct xfs_bmap *key, xfs_bmap_iter_fn fn,
		void *arg);

#endif /* XFS_SCRUB_FILEMAP_H_ */
