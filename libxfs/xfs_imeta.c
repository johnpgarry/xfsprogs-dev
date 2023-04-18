// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_imeta.h"
#include "xfs_trace.h"
#include "xfs_inode.h"
#include "xfs_ialloc.h"
#include "xfs_bmap_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_trans_space.h"
#include "xfs_ag.h"

/*
 * Metadata File Management
 * ========================
 *
 * These functions provide an abstraction layer for looking up, creating, and
 * deleting metadata inodes.  These pointers live in the in-core superblock,
 * so the functions moderate access to those fields and take care of logging.
 *
 * For the five existing metadata inodes (real time bitmap & summary; and the
 * user, group, and quotas) we'll continue to maintain the in-core superblock
 * inodes for reads and only require xfs_imeta_create and xfs_imeta_unlink to
 * persist changes.  New metadata inode types must only use the xfs_imeta_*
 * functions.
 *
 * Callers wishing to create or unlink a metadata inode must pass in a
 * xfs_imeta_end structure.  After committing or cancelling the transaction,
 * this structure must be passed to xfs_imeta_end_update to free resources that
 * cannot be freed during the transaction.
 *
 * Right now we only support callers passing in the predefined metadata inode
 * paths; the goal is that callers will some day locate metadata inodes based
 * on path lookups into a metadata directory structure.
 */

/* Static metadata inode paths */
static const unsigned char *rtbitmap_path[]	= {"realtime", "bitmap"};
static const unsigned char *rtsummary_path[]	= {"realtime", "summary"};
static const unsigned char *usrquota_path[]	= {"quota", "user"};
static const unsigned char *grpquota_path[]	= {"quota", "group"};
static const unsigned char *prjquota_path[]	= {"quota", "project"};

XFS_IMETA_DEFINE_PATH(XFS_IMETA_RTBITMAP,	rtbitmap_path);
XFS_IMETA_DEFINE_PATH(XFS_IMETA_RTSUMMARY,	rtsummary_path);
XFS_IMETA_DEFINE_PATH(XFS_IMETA_USRQUOTA,	usrquota_path);
XFS_IMETA_DEFINE_PATH(XFS_IMETA_GRPQUOTA,	grpquota_path);
XFS_IMETA_DEFINE_PATH(XFS_IMETA_PRJQUOTA,	prjquota_path);

/* Are these two paths equal? */
STATIC bool
xfs_imeta_path_compare(
	const struct xfs_imeta_path	*a,
	const struct xfs_imeta_path	*b)
{
	unsigned int			i;

	if (a == b)
		return true;

	if (a->im_depth != b->im_depth)
		return false;

	for (i = 0; i < a->im_depth; i++)
		if (a->im_path[i] != b->im_path[i] &&
		    strcmp(a->im_path[i], b->im_path[i]))
			return false;

	return true;
}

/* Is this path ok? */
static inline bool
xfs_imeta_path_check(
	const struct xfs_imeta_path	*path)
{
	return path->im_depth <= XFS_IMETA_MAX_DEPTH;
}

/* Functions for storing and retrieving superblock inode values. */

/* Mapping of metadata inode paths to in-core superblock values. */
static const struct xfs_imeta_sbmap {
	const struct xfs_imeta_path	*path;
	unsigned int			offset;
} xfs_imeta_sbmaps[] = {
	{
		.path	= &XFS_IMETA_RTBITMAP,
		.offset	= offsetof(struct xfs_sb, sb_rbmino),
	},
	{
		.path	= &XFS_IMETA_RTSUMMARY,
		.offset	= offsetof(struct xfs_sb, sb_rsumino),
	},
	{
		.path	= &XFS_IMETA_USRQUOTA,
		.offset	= offsetof(struct xfs_sb, sb_uquotino),
	},
	{
		.path	= &XFS_IMETA_GRPQUOTA,
		.offset	= offsetof(struct xfs_sb, sb_gquotino),
	},
	{
		.path	= &XFS_IMETA_PRJQUOTA,
		.offset	= offsetof(struct xfs_sb, sb_pquotino),
	},
	{ NULL, 0 },
};

/* Return a pointer to the in-core superblock inode value. */
static inline xfs_ino_t *
xfs_imeta_sbmap_to_inop(
	struct xfs_mount		*mp,
	const struct xfs_imeta_sbmap	*map)
{
	return (xfs_ino_t *)(((char *)&mp->m_sb) + map->offset);
}

/* Compute location of metadata inode pointer in the in-core superblock */
static inline xfs_ino_t *
xfs_imeta_path_to_sb_inop(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path)
{
	const struct xfs_imeta_sbmap	*p;

	for (p = xfs_imeta_sbmaps; p->path; p++)
		if (xfs_imeta_path_compare(p->path, path))
			return xfs_imeta_sbmap_to_inop(mp, p);

	return NULL;
}

/* Look up a superblock metadata inode by its path. */
STATIC int
xfs_imeta_sb_lookup(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	xfs_ino_t			*inop)
{
	xfs_ino_t			*sb_inop;

	sb_inop = xfs_imeta_path_to_sb_inop(mp, path);
	if (!sb_inop)
		return -EINVAL;

	trace_xfs_imeta_sb_lookup(mp, sb_inop);
	*inop = *sb_inop;
	return 0;
}

/* Update inode pointers in the superblock. */
static inline void
xfs_imeta_log_sb(
	struct xfs_trans	*tp)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_buf		*bp = xfs_trans_getsb(tp);

	/*
	 * Update the inode flags in the ondisk superblock without touching
	 * the summary counters.  We have not quiesced inode chunk allocation,
	 * so we cannot coordinate with updates to the icount and ifree percpu
	 * counters.
	 */
	xfs_sb_to_disk(bp->b_addr, &mp->m_sb);
	xfs_trans_buf_set_type(tp, bp, XFS_BLFT_SB_BUF);
	xfs_trans_log_buf(tp, bp, 0, sizeof(struct xfs_dsb) - 1);
}

/*
 * Create a new metadata inode and set a superblock pointer to this new inode.
 * The superblock field must not already be pointing to an inode.
 */
STATIC int
xfs_imeta_sb_create(
	struct xfs_imeta_update		*upd,
	umode_t				mode)
{
	struct xfs_icreate_args		args = {
		.nlink			= S_ISDIR(mode) ? 2 : 1,
	};
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			*sb_inop;
	xfs_ino_t			ino;
	int				error;

	/* Files rooted in the superblock do not have parents. */
	xfs_icreate_args_rootfile(&args, mp, mode, false);

	/* Reject if the sb already points to some inode. */
	sb_inop = xfs_imeta_path_to_sb_inop(mp, upd->path);
	if (!sb_inop)
		return -EINVAL;

	if (*sb_inop != NULLFSINO)
		return -EEXIST;

	/* Create a new inode and set the sb pointer. */
	error = xfs_dialloc(&upd->tp, 0, mode, &ino);
	if (error)
		return error;
	error = xfs_icreate(upd->tp, ino, &args, &upd->ip);
	if (error)
		return error;
	upd->ip_locked = true;

	/*
	 * If we ever need the ability to create rt metadata files on a
	 * pre-metadir filesystem, we'll need to dqattach the child here.
	 * Currently we assume that mkfs will create the files and quotacheck
	 * will account for them.
	 */

	/* Update superblock pointer. */
	*sb_inop = ino;
	xfs_imeta_log_sb(upd->tp);

	trace_xfs_imeta_sb_create(upd);
	return 0;
}

/*
 * Clear the given inode pointer from the superblock and drop the link count
 * of the metadata inode.
 */
STATIC int
xfs_imeta_sb_unlink(
	struct xfs_imeta_update		*upd)
{
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			*sb_inop;

	ASSERT(xfs_isilocked(upd->ip, XFS_ILOCK_EXCL));

	sb_inop = xfs_imeta_path_to_sb_inop(mp, upd->path);
	if (!sb_inop)
		return -EINVAL;

	/* Reject if the sb doesn't point to the inode that was passed in. */
	if (*sb_inop != upd->ip->i_ino)
		return -ENOENT;

	trace_xfs_imeta_sb_unlink(upd);

	*sb_inop = NULLFSINO;
	xfs_imeta_log_sb(upd->tp);
	return xfs_droplink(upd->tp, upd->ip);
}

/* Set the given inode pointer in the superblock. */
STATIC int
xfs_imeta_sb_link(
	struct xfs_imeta_update		*upd)
{
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			*sb_inop;

	ASSERT(xfs_isilocked(upd->ip, XFS_ILOCK_EXCL));

	sb_inop = xfs_imeta_path_to_sb_inop(mp, upd->path);
	if (!sb_inop)
		return -EINVAL;
	if (*sb_inop != NULLFSINO)
		return -EEXIST;

	trace_xfs_imeta_sb_link(upd);

	xfs_bumplink(upd->tp, upd->ip);
	xfs_imeta_log_sb(upd->tp);

	*sb_inop = upd->ip->i_ino;
	return 0;
}

/* General functions for managing metadata inode pointers */

/*
 * Is this metadata inode pointer ok?  We allow the fields to be set to
 * NULLFSINO if the metadata structure isn't present, and we don't allow
 * obviously incorrect inode pointers.
 */
static inline bool
xfs_imeta_verify(
	struct xfs_mount	*mp,
	xfs_ino_t		ino)
{
	if (ino == NULLFSINO)
		return true;
	return xfs_verify_ino(mp, ino);
}

/* Look up a metadata inode by its path. */
int
xfs_imeta_lookup(
	struct xfs_trans		*tp,
	const struct xfs_imeta_path	*path,
	xfs_ino_t			*inop)
{
	struct xfs_mount		*mp = tp->t_mountp;
	xfs_ino_t			ino;
	int				error;

	ASSERT(xfs_imeta_path_check(path));

	error = xfs_imeta_sb_lookup(mp, path, &ino);
	if (error)
		return error;

	if (!xfs_imeta_verify(mp, ino))
		return -EFSCORRUPTED;

	*inop = ino;
	return 0;
}

/*
 * Create a metadata inode with the given @mode, and insert it into the
 * metadata directory tree at the given @path.  The path (up to the final
 * component) must already exist.
 *
 * The new metadata inode will be attached to the update structure @upd->ip,
 * with the ILOCK held until the caller releases it.  @ipp is set to upd->ip
 * as a convenience for callers.
 *
 * Callers must ensure that the root dquots are allocated, if applicable.
 *
 * NOTE: This function may return a new inode to the caller even if it returns
 * a negative error code.  If an inode is passed back, the caller must finish
 * setting up the inode before releasing it.
 */
int
xfs_imeta_create(
	struct xfs_imeta_update		*upd,
	umode_t				mode,
	struct xfs_inode		**ipp)
{
	int				error;

	ASSERT(xfs_imeta_path_check(upd->path));

	*ipp = NULL;

	error = xfs_imeta_sb_create(upd, mode);
	*ipp = upd->ip;
	return error;
}

/* Free a file from the metadata directory tree. */
STATIC int
xfs_imeta_ifree(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_perag	*pag;
	struct xfs_icluster	xic = { 0 };
	int			error;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(VFS_I(ip)->i_nlink == 0);
	ASSERT(ip->i_df.if_nextents == 0);
	ASSERT(ip->i_disk_size == 0 || !S_ISREG(VFS_I(ip)->i_mode));
	ASSERT(ip->i_nblocks == 0);

	pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ip->i_ino));

	error = xfs_dir_ifree(tp, pag, ip, &xic);
	if (error)
		goto out;

	/* Metadata files do not support ownership changes or DMAPI. */

	if (xic.deleted)
		error = xfs_ifree_cluster(tp, pag, ip, &xic);
out:
	xfs_perag_put(pag);
	return error;
}

/*
 * Unlink a metadata inode @upd->ip from the metadata directory given by @path.
 * The path must already exist.
 */
int
xfs_imeta_unlink(
	struct xfs_imeta_update		*upd)
{
	int				error;

	ASSERT(xfs_imeta_path_check(upd->path));
	ASSERT(xfs_imeta_verify(upd->mp, upd->ip->i_ino));

	error = xfs_imeta_sb_unlink(upd);
	if (error)
		return error;

	/*
	 * Metadata files require explicit resource cleanup.  In other words,
	 * the inactivation system will not touch these files, so we must free
	 * the ondisk inode by ourselves if warranted.
	 */
	if (VFS_I(upd->ip)->i_nlink > 0)
		return 0;

	return xfs_imeta_ifree(upd->tp, upd->ip);
}

/*
 * Link the metadata directory given by @path to the inode @upd->ip.
 * The path (up to the final component) must already exist, but the final
 * component must not already exist.
 */
int
xfs_imeta_link(
	struct xfs_imeta_update		*upd)
{
	ASSERT(xfs_imeta_path_check(upd->path));

	return xfs_imeta_sb_link(upd);
}

/* Does this inode number refer to a static metadata inode? */
bool
xfs_is_static_meta_ino(
	struct xfs_mount		*mp,
	xfs_ino_t			ino)
{
	const struct xfs_imeta_sbmap	*p;

	if (ino == NULLFSINO)
		return false;

	for (p = xfs_imeta_sbmaps; p->path; p++)
		if (ino == *xfs_imeta_sbmap_to_inop(mp, p))
			return true;

	return false;
}

/*
 * Ensure that the in-core superblock has all the values that it should.
 * Caller should pass in an empty transaction to avoid livelocking on btree
 * cycles.
 */
int
xfs_imeta_mount(
	struct xfs_trans	*tp)
{
	return 0;
}
