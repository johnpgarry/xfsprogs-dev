// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_VFS_H_
#define XFS_SCRUB_VFS_H_

/*
 * Visit a subdirectory prior to iterating entries in that subdirectory.
 * Return true to continue iteration or false to stop iterating and return to
 * the caller.
 */
typedef bool (*scan_fs_tree_dir_fn)(struct scrub_ctx *, const char *,
		int, void *);

/*
 * Visit each directory entry in a directory.  Return true to continue
 * iteration or false to stop iterating and return to the caller.
 */
typedef bool (*scan_fs_tree_dirent_fn)(struct scrub_ctx *, const char *,
		int, struct dirent *, struct stat *, void *);

bool scan_fs_tree(struct scrub_ctx *ctx, scan_fs_tree_dir_fn dir_fn,
		scan_fs_tree_dirent_fn dirent_fn, void *arg);

void fstrim(struct scrub_ctx *ctx);

#endif /* XFS_SCRUB_VFS_H_ */
