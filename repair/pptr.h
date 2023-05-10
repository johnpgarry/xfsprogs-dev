/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __REPAIR_PPTR_H__
#define __REPAIR_PPTR_H__

void parent_ptr_free(struct xfs_mount *mp);
void parent_ptr_init(struct xfs_mount *mp);

void add_parent_ptr(xfs_ino_t ino, const unsigned char *fname,
		struct xfs_inode *dp);

void check_parent_ptrs(struct xfs_mount *mp);

#endif /* __REPAIR_PPTR_H__ */
