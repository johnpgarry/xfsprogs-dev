/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_IMETA_H__
#define __XFS_IMETA_H__

/* How deep can we nest metadata dirs? */
#define XFS_IMETA_MAX_DEPTH	64

/* Form an imeta path from a simple array of strings. */
#define XFS_IMETA_DEFINE_PATH(name, path) \
const struct xfs_imeta_path name = { \
	.im_path = (path), \
	.im_ftype = XFS_DIR3_FT_REG_FILE, \
	.im_depth = ARRAY_SIZE(path), \
}

/* Key for looking up metadata inodes. */
struct xfs_imeta_path {
	/* Array of string pointers. */
	const unsigned char	**im_path;

	/* Number of strings in path. */
	uint8_t			im_depth;

	/* Expected file type. */
	uint8_t			im_ftype;
};

/* Cleanup widget for metadata inode creation and deletion. */
struct xfs_imeta_update {
	struct xfs_mount	*mp;
	struct xfs_trans	*tp;

	const struct xfs_imeta_path *path;

	/* Parent pointer update context */
	struct xfs_parent_args	*ppargs;

	/* Parent directory */
	struct xfs_inode	*dp;

	/* Metadata inode */
	struct xfs_inode	*ip;

	unsigned int		dp_locked:1;
	unsigned int		ip_locked:1;
};

/* Grab the last path component, mostly for tracing. */
static inline const unsigned char *
xfs_imeta_lastpath(
	const struct xfs_imeta_update	*upd)
{
	if (upd->path && upd->path->im_path && upd->path->im_depth > 0)
		return upd->path->im_path[upd->path->im_depth - 1];
	return "?";
}

/* Lookup keys for static metadata inodes. */
extern const struct xfs_imeta_path XFS_IMETA_RTBITMAP;
extern const struct xfs_imeta_path XFS_IMETA_RTSUMMARY;
extern const struct xfs_imeta_path XFS_IMETA_USRQUOTA;
extern const struct xfs_imeta_path XFS_IMETA_GRPQUOTA;
extern const struct xfs_imeta_path XFS_IMETA_PRJQUOTA;
extern const struct xfs_imeta_path XFS_IMETA_METADIR;

int xfs_imeta_lookup(struct xfs_trans *tp, const struct xfs_imeta_path *path,
		xfs_ino_t *ino);
int xfs_imeta_dir_parent(struct xfs_trans *tp,
		const struct xfs_imeta_path *path, struct xfs_inode **dpp);

void xfs_imeta_set_iflag(struct xfs_trans *tp, struct xfs_inode *ip);
void xfs_imeta_clear_iflag(struct xfs_trans *tp, struct xfs_inode *ip);

int xfs_imeta_create(struct xfs_imeta_update *upd, umode_t mode,
		struct xfs_inode **ipp);
int xfs_imeta_unlink(struct xfs_imeta_update *upd);
int xfs_imeta_link(struct xfs_imeta_update *upd);

bool xfs_is_static_meta_ino(struct xfs_mount *mp, xfs_ino_t ino);
int xfs_imeta_mount(struct xfs_trans *tp);

unsigned int xfs_imeta_create_space_res(struct xfs_mount *mp);
unsigned int xfs_imeta_link_space_res(struct xfs_mount *mp);
unsigned int xfs_imeta_unlink_space_res(struct xfs_mount *mp);

/* Must be implemented by the libxfs client */
int xfs_imeta_iget(struct xfs_trans *tp, xfs_ino_t ino, unsigned char ftype,
		struct xfs_inode **ipp);
void xfs_imeta_irele(struct xfs_inode *ip);

#endif /* __XFS_IMETA_H__ */
