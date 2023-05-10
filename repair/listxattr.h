/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __REPAIR_LISTXATTR_H__
#define __REPAIR_LISTXATTR_H__

typedef int (*xattr_walk_fn)(struct xfs_inode *ip, unsigned int attr_flags,
		const unsigned char *name, unsigned int namelen,
		const void *value, unsigned int valuelen, void *priv);

int xattr_walk(struct xfs_inode *ip, xattr_walk_fn attr_fn, void *priv);

#endif /* __REPAIR_LISTXATTR_H__ */
