/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_IMETA_H__
#define __XFS_IMETA_H__

/* Key for looking up metadata inodes. */
struct xfs_imeta_path {
	/* Temporary: integer to keep the static imeta definitions unique */
	int		bogus;
};

/* Cleanup widget for metadata inode creation and deletion. */
struct xfs_imeta_update {
	struct xfs_mount	*mp;
	struct xfs_trans	*tp;

	const struct xfs_imeta_path *path;

	/* Metadata inode */
	struct xfs_inode	*ip;

	unsigned int		ip_locked:1;
};

/* Lookup keys for static metadata inodes. */
extern const struct xfs_imeta_path XFS_IMETA_RTBITMAP;
extern const struct xfs_imeta_path XFS_IMETA_RTSUMMARY;
extern const struct xfs_imeta_path XFS_IMETA_USRQUOTA;
extern const struct xfs_imeta_path XFS_IMETA_GRPQUOTA;
extern const struct xfs_imeta_path XFS_IMETA_PRJQUOTA;

int xfs_imeta_lookup(struct xfs_trans *tp, const struct xfs_imeta_path *path,
		xfs_ino_t *ino);

int xfs_imeta_create(struct xfs_imeta_update *upd, umode_t mode,
		struct xfs_inode **ipp);
int xfs_imeta_unlink(struct xfs_imeta_update *upd);
int xfs_imeta_link(struct xfs_imeta_update *upd);

bool xfs_is_static_meta_ino(struct xfs_mount *mp, xfs_ino_t ino);
int xfs_imeta_mount(struct xfs_trans *tp);

unsigned int xfs_imeta_create_space_res(struct xfs_mount *mp);
unsigned int xfs_imeta_link_space_res(struct xfs_mount *mp);
unsigned int xfs_imeta_unlink_space_res(struct xfs_mount *mp);

#endif /* __XFS_IMETA_H__ */
