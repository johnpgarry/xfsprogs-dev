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
	/* buffer cache target for this in-memory btree */
	struct xfs_buftarg		*target;

	/* Owner of this btree. */
	unsigned long long		owner;

	/* Btree header */
	union xfs_btree_ptr		root;
	unsigned int			nlevels;
};

#endif /* CONFIG_XFS_BTREE_IN_XFILE */

#endif /* __LIBXFS_XFBTREE_H__ */
