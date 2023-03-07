/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBXFS_XFBTREE_H__
#define __LIBXFS_XFBTREE_H__

#ifdef CONFIG_XFS_BTREE_IN_XFILE

/* xfile-backed in-memory btrees */

struct xfbtree {
	/* buffer cache target for the xfile backing this in-memory btree */
	struct xfs_buftarg		*target;

	/* Bitmap of free space from pos to used */
	struct bitmap			*freespace;

	/* Highest xfile offset that has been written to. */
	xfileoff_t			highest_offset;

	/* Owner of this btree. */
	unsigned long long		owner;

	/* Btree header */
	union xfs_btree_ptr		root;
	unsigned int			nlevels;

	/* Minimum and maximum records per block. */
	unsigned int			maxrecs[2];
	unsigned int			minrecs[2];
};

void xfbtree_destroy(struct xfbtree *xfbt);
int xfbtree_trans_commit(struct xfbtree *xfbt, struct xfs_trans *tp);
void xfbtree_trans_cancel(struct xfbtree *xfbt, struct xfs_trans *tp);

#endif /* CONFIG_XFS_BTREE_IN_XFILE */

#endif /* __LIBXFS_XFBTREE_H__ */
