/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_IMETA_UTILS_H__
#define __XFS_IMETA_UTILS_H__

int xfs_imeta_start_create(struct xfs_mount *mp,
		const struct xfs_imeta_path *path,
		struct xfs_imeta_update *upd);

int xfs_imeta_start_link(struct xfs_mount *mp,
		const struct xfs_imeta_path *path,
		struct xfs_inode *ip, struct xfs_imeta_update *upd);

int xfs_imeta_start_unlink(struct xfs_mount *mp,
		const struct xfs_imeta_path *path,
		struct xfs_inode *ip, struct xfs_imeta_update *upd);

int xfs_imeta_ensure_dirpath(struct xfs_mount *mp,
		const struct xfs_imeta_path *path);

int xfs_imeta_commit_update(struct xfs_imeta_update *upd);
void xfs_imeta_cancel_update(struct xfs_imeta_update *upd, int error);

#endif /* __XFS_IMETA_UTILS_H__ */


