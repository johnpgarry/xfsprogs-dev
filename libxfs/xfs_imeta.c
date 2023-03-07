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
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_health.h"
#include "xfs_errortag.h"
#include "xfs_btree.h"
#include "xfs_alloc.h"

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
 * When the metadata directory tree (metadir) feature is enabled, we can create
 * a complex directory tree in which to store metadata inodes.  Inodes within
 * the metadata directory tree should have the "metadata" inode flag set to
 * prevent them from being exposed to the outside world.
 *
 * Callers are not expected to take the IOLOCK of metadata directories.  They
 * are expected to take the ILOCK of any inode in the metadata directory tree
 * (just like the regular to synchronize access to that inode.  It is not
 * necessary to take the MMAPLOCK since metadata inodes should never be exposed
 * to user space.
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

const struct xfs_imeta_path XFS_IMETA_METADIR = {
	.im_depth = 0,
	.im_ftype = XFS_DIR3_FT_DIR,
};

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
	{
		.path	= &XFS_IMETA_METADIR,
		.offset	= offsetof(struct xfs_sb, sb_metadirino),
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
	error = xfs_dialloc(&upd->tp, NULL, mode, &ino);
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

/* Functions for storing and retrieving metadata directory inode values. */

static inline void
xfs_imeta_set_xname(
	struct xfs_name			*xname,
	const struct xfs_imeta_path	*path,
	unsigned int			path_idx,
	unsigned char			ftype)
{
	xname->name = (const unsigned char *)path->im_path[path_idx];
	xname->len = strlen(path->im_path[path_idx]);
	xname->type = ftype;
}

/*
 * Look up the inode number and filetype for an exact name in a directory.
 * Caller must hold ILOCK_EXCL.
 */
static inline int
xfs_imeta_dir_lookup(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	struct xfs_name		*xname,
	xfs_ino_t		*ino)
{
	struct xfs_da_args	args = {
		.trans		= tp,
		.dp		= dp,
		.geo		= dp->i_mount->m_dir_geo,
		.name		= xname->name,
		.namelen	= xname->len,
		.hashval	= xfs_dir2_hashname(dp->i_mount, xname),
		.whichfork	= XFS_DATA_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
		.owner		= dp->i_ino,
	};
	bool			isblock, isleaf;
	int			error;

	if (xfs_is_shutdown(dp->i_mount))
		return -EIO;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		error = xfs_dir2_sf_lookup(&args);
		goto out_unlock;
	}

	/* dir2 functions require that the data fork is loaded */
	error = xfs_iread_extents(tp, dp, XFS_DATA_FORK);
	if (error)
		goto out_unlock;

	error = xfs_dir2_isblock(&args, &isblock);
	if (error)
		goto out_unlock;

	if (isblock) {
		error = xfs_dir2_block_lookup(&args);
		goto out_unlock;
	}

	error = xfs_dir2_isleaf(&args, &isleaf);
	if (error)
		goto out_unlock;

	if (isleaf) {
		error = xfs_dir2_leaf_lookup(&args);
		goto out_unlock;
	}

	error = xfs_dir2_node_lookup(&args);

out_unlock:
	if (error == -EEXIST)
		error = 0;
	if (error)
		return error;

	*ino = args.inumber;
	xname->type = args.filetype;
	return 0;
}

/*
 * Given a parent directory @dp and a metadata inode path component @xname,
 * Look up the inode number in the directory, returning it in @ino.
 * @xname.type must match the directory entry's ftype.
 *
 * Caller must hold ILOCK_EXCL.
 */
static inline int
xfs_imeta_dir_lookup_component(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	struct xfs_name		*xname,
	xfs_ino_t		*ino)
{
	int			type_wanted = xname->type;
	int			error;

	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		xfs_fs_mark_sick(dp->i_mount, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	error = xfs_imeta_dir_lookup(tp, dp, xname, ino);
	if (error)
		return error;
	if (!xfs_verify_ino(dp->i_mount, *ino)) {
		xfs_fs_mark_sick(dp->i_mount, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}
	if (type_wanted != XFS_DIR3_FT_UNKNOWN && xname->type != type_wanted) {
		xfs_fs_mark_sick(dp->i_mount, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	trace_xfs_imeta_dir_lookup(dp, xname, *ino);
	return 0;
}

/*
 * Traverse a metadata directory tree path, returning the inode corresponding
 * to the parent of the last path component.  If any of the path components do
 * not exist, return -ENOENT.  Caller must supply a transaction to avoid
 * livelocks on btree cycles.
 *
 * @dp is returned without any locks held.
 */
int
xfs_imeta_dir_parent(
	struct xfs_trans		*tp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		**dpp)
{
	struct xfs_name			xname;
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_inode		*dp = NULL;
	xfs_ino_t			ino;
	unsigned int			i;
	int				error;

	/* Caller wanted the root, we're done! */
	if (path->im_depth == 0)
		goto out;

	/* No metadata directory means no parent. */
	if (mp->m_metadirip == NULL)
		return -ENOENT;

	/* Grab a new reference to the metadir root dir. */
	error = xfs_imeta_iget(tp, mp->m_metadirip->i_ino, XFS_DIR3_FT_DIR,
			&dp);
	if (error)
		return error;

	for (i = 0; i < path->im_depth - 1; i++) {
		struct xfs_inode	*ip = NULL;

		xfs_ilock(dp, XFS_ILOCK_EXCL);

		/* Look up the name in the current directory. */
		xfs_imeta_set_xname(&xname, path, i, XFS_DIR3_FT_DIR);
		error = xfs_imeta_dir_lookup_component(tp, dp, &xname, &ino);
		if (error)
			goto out_rele;

		/*
		 * Grab the child inode while we still have the parent
		 * directory locked.
		 */
		error = xfs_imeta_iget(tp, ino, XFS_DIR3_FT_DIR, &ip);
		if (error)
			goto out_rele;

		xfs_iunlock(dp, XFS_ILOCK_EXCL);
		xfs_imeta_irele(dp);
		dp = ip;
	}

out:
	*dpp = dp;
	return 0;

out_rele:
	xfs_iunlock(dp, XFS_ILOCK_EXCL);
	xfs_imeta_irele(dp);
	return error;
}

/*
 * Look up a metadata inode from the metadata directory.  If the last path
 * component doesn't exist, return NULLFSINO.  If any other part of the path
 * does not exist, return -ENOENT so we can distinguish the two.
 */
STATIC int
xfs_imeta_dir_lookup_int(
	struct xfs_trans		*tp,
	const struct xfs_imeta_path	*path,
	xfs_ino_t			*inop)
{
	struct xfs_name			xname;
	struct xfs_inode		*dp = NULL;
	xfs_ino_t			ino;
	int				error;

	/* metadir ino is recorded in superblock */
	if (xfs_imeta_path_compare(path, &XFS_IMETA_METADIR))
		return xfs_imeta_sb_lookup(tp->t_mountp, path, inop);

	ASSERT(path->im_depth > 0);

	/* Find the parent of the last path component. */
	error = xfs_imeta_dir_parent(tp, path, &dp);
	if (error)
		return error;

	xfs_ilock(dp, XFS_ILOCK_EXCL);

	/* Look up the name in the current directory. */
	xfs_imeta_set_xname(&xname, path, path->im_depth - 1, path->im_ftype);
	error = xfs_imeta_dir_lookup_component(tp, dp, &xname, &ino);
	switch (error) {
	case 0:
		*inop = ino;
		break;
	case -ENOENT:
		*inop = NULLFSINO;
		error = 0;
		break;
	}

	xfs_iunlock(dp, XFS_ILOCK_EXCL);
	xfs_imeta_irele(dp);
	return error;
}

/*
 * Load all the metadata inode pointers that are cached in the in-core
 * superblock but live somewhere in the metadata directory tree.
 */
STATIC int
xfs_imeta_dir_mount(
	struct xfs_trans		*tp)
{
	struct xfs_mount		*mp = tp->t_mountp;
	const struct xfs_imeta_sbmap	*p;
	xfs_ino_t			*sb_inop;
	int				err2;
	int				error = 0;

	for (p = xfs_imeta_sbmaps; p->path && p->path->im_depth > 0; p++) {
		if (p->path == &XFS_IMETA_METADIR)
			continue;
		sb_inop = xfs_imeta_sbmap_to_inop(mp, p);
		err2 = xfs_imeta_dir_lookup_int(tp, p->path, sb_inop);
		if (err2 == -ENOENT) {
			*sb_inop = NULLFSINO;
			continue;
		}
		if (!error && err2)
			error = err2;
	}

	return error;
}

/* Set up an inode to be recognized as a metadata directory inode. */
void
xfs_imeta_set_iflag(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	VFS_I(ip)->i_mode &= ~0777;
	VFS_I(ip)->i_uid = GLOBAL_ROOT_UID;
	VFS_I(ip)->i_gid = GLOBAL_ROOT_GID;
	ip->i_projid = 0;
	ip->i_diflags |= (XFS_DIFLAG_IMMUTABLE | XFS_DIFLAG_SYNC |
			  XFS_DIFLAG_NOATIME | XFS_DIFLAG_NODUMP |
			  XFS_DIFLAG_NODEFRAG);
	if (S_ISDIR(VFS_I(ip)->i_mode))
		ip->i_diflags |= XFS_DIFLAG_NOSYMLINKS;
	ip->i_diflags2 &= ~XFS_DIFLAG2_DAX;
	ip->i_diflags2 |= XFS_DIFLAG2_METADIR;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}


/* Clear the metadata directory inode flag. */
void
xfs_imeta_clear_iflag(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	ASSERT(xfs_is_metadir_inode(ip));
	ASSERT(VFS_I(ip)->i_nlink == 0);

	ip->i_diflags2 &= ~XFS_DIFLAG2_METADIR;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}
/*
 * Create a new metadata inode accessible via the given metadata directory path.
 * Callers must ensure that the directory entry does not already exist; a new
 * one will be created.
 */
STATIC int
xfs_imeta_dir_create(
	struct xfs_imeta_update		*upd,
	umode_t				mode)
{
	struct xfs_icreate_args		args = {
		.pip			= upd->dp,
		.nlink			= S_ISDIR(mode) ? 2 : 1,
	};
	struct xfs_name			xname;
	struct xfs_dir_update		du = {
		.dp			= upd->dp,
		.name			= &xname,
		.ppargs			= upd->ppargs,
	};
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			*sb_inop;
	xfs_ino_t			ino;
	unsigned int			resblks;
	int				error;

	ASSERT(xfs_isilocked(upd->dp, XFS_ILOCK_EXCL));

	/* metadir ino is recorded in superblock; only mkfs gets to do this */
	if (xfs_imeta_path_compare(upd->path, &XFS_IMETA_METADIR)) {
		error = xfs_imeta_sb_create(upd, mode);
		if (error)
			return error;

		/* Set the metadata iflag, initialize directory. */
		xfs_imeta_set_iflag(upd->tp, upd->ip);
		return xfs_dir_init(upd->tp, upd->ip, upd->ip);
	}

	ASSERT(upd->path->im_depth > 0);

	xfs_icreate_args_rootfile(&args, mp, mode, xfs_has_parent(mp));

	/* Check that the name does not already exist in the directory. */
	xfs_imeta_set_xname(&xname, upd->path, upd->path->im_depth - 1,
			XFS_DIR3_FT_UNKNOWN);
	error = xfs_imeta_dir_lookup_component(upd->tp, upd->dp, &xname, &ino);
	switch (error) {
	case -ENOENT:
		break;
	case 0:
		error = -EEXIST;
		fallthrough;
	default:
		return error;
	}

	/*
	 * A newly created regular or special file just has one directory
	 * entry pointing to them, but a directory also the "." entry
	 * pointing to itself.
	 */
	error = xfs_dialloc(&upd->tp, upd->dp, mode, &ino);
	if (error)
		return error;
	error = xfs_icreate(upd->tp, ino, &args, &upd->ip);
	if (error)
		return error;
	du.ip = upd->ip;
	xfs_imeta_set_iflag(upd->tp, upd->ip);
	upd->ip_locked = true;

	/*
	 * Join the directory inode to the transaction.  We do not do it
	 * earlier because xfs_dialloc rolls the transaction.
	 */
	xfs_trans_ijoin(upd->tp, upd->dp, 0);

	/* Create the entry. */
	if (S_ISDIR(args.mode))
		resblks = xfs_mkdir_space_res(mp, xname.len);
	else
		resblks = xfs_create_space_res(mp, xname.len);
	xname.type = xfs_mode_to_ftype(args.mode);

	trace_xfs_imeta_dir_try_create(upd);

	error = xfs_dir_create_child(upd->tp, resblks, &du);
	if (error)
		return error;

	/* Metadir files are not accounted to quota. */

	trace_xfs_imeta_dir_create(upd);

	/* Update the in-core superblock value if there is one. */
	sb_inop = xfs_imeta_path_to_sb_inop(mp, upd->path);
	if (sb_inop)
		*sb_inop = ino;
	return 0;
}

/*
 * Remove the given entry from the metadata directory and drop the link count
 * of the metadata inode.
 */
STATIC int
xfs_imeta_dir_unlink(
	struct xfs_imeta_update		*upd)
{
	struct xfs_name			xname;
	struct xfs_dir_update		du = {
		.dp			= upd->dp,
		.name			= &xname,
		.ip			= upd->ip,
		.ppargs			= upd->ppargs,
	};
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			*sb_inop;
	xfs_ino_t			ino;
	unsigned int			resblks;
	int				error;

	ASSERT(xfs_isilocked(upd->dp, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(upd->ip, XFS_ILOCK_EXCL));

	/* Metadata directory root cannot be unlinked. */
	if (xfs_imeta_path_compare(upd->path, &XFS_IMETA_METADIR)) {
		ASSERT(0);
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	ASSERT(upd->path->im_depth > 0);

	/* Look up the name in the current directory. */
	xfs_imeta_set_xname(&xname, upd->path, upd->path->im_depth - 1,
			xfs_mode_to_ftype(VFS_I(upd->ip)->i_mode));
	error = xfs_imeta_dir_lookup_component(upd->tp, upd->dp, &xname, &ino);
	switch (error) {
	case 0:
		if (ino != upd->ip->i_ino)
			error = -ENOENT;
		break;
	case -ENOENT:
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		error = -EFSCORRUPTED;
		break;
	}
	if (error)
		return error;

	resblks = xfs_remove_space_res(mp, xname.len);
	error = xfs_dir_remove_child(upd->tp, resblks, &du);
	if (error)
		return error;

	trace_xfs_imeta_dir_unlink(upd);

	/* Update the in-core superblock value if there is one. */
	sb_inop = xfs_imeta_path_to_sb_inop(mp, upd->path);
	if (sb_inop)
		*sb_inop = NULLFSINO;
	return 0;
}

/* Set the given path in the metadata directory to point to an inode. */
STATIC int
xfs_imeta_dir_link(
	struct xfs_imeta_update		*upd)
{
	struct xfs_name			xname;
	struct xfs_dir_update		du = {
		.dp			= upd->dp,
		.name			= &xname,
		.ip			= upd->ip,
		.ppargs			= upd->ppargs,
	};
	struct xfs_mount		*mp = upd->mp;
	xfs_ino_t			*sb_inop;
	xfs_ino_t			ino;
	unsigned int			resblks;
	int				error;

	ASSERT(xfs_isilocked(upd->dp, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(upd->ip, XFS_ILOCK_EXCL));

	/* Metadata directory root cannot be linked. */
	if (xfs_imeta_path_compare(upd->path, &XFS_IMETA_METADIR)) {
		ASSERT(0);
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

	ASSERT(upd->path->im_depth > 0);

	/* Look up the name in the current directory. */
	xfs_imeta_set_xname(&xname, upd->path, upd->path->im_depth - 1,
			xfs_mode_to_ftype(VFS_I(upd->ip)->i_mode));
	error = xfs_imeta_dir_lookup_component(upd->tp, upd->dp, &xname, &ino);
	switch (error) {
	case -ENOENT:
		break;
	case 0:
		error = -EEXIST;
		fallthrough;
	default:
		return error;
	}

	resblks = xfs_link_space_res(mp, xname.len);
	error = xfs_dir_add_child(upd->tp, resblks, &du);
	if (error)
		return error;

	trace_xfs_imeta_dir_link(upd);

	/* Update the in-core superblock value if there is one. */
	sb_inop = xfs_imeta_path_to_sb_inop(mp, upd->path);
	if (sb_inop)
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

	if (xfs_has_metadir(mp)) {
		error = xfs_imeta_dir_lookup_int(tp, path, &ino);
		if (error == -ENOENT) {
			xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
			return -EFSCORRUPTED;
		}
	} else {
		error = xfs_imeta_sb_lookup(mp, path, &ino);
	}
	if (error)
		return error;

	if (!xfs_imeta_verify(mp, ino)) {
		xfs_fs_mark_sick(mp, XFS_SICK_FS_METADIR);
		return -EFSCORRUPTED;
	}

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
	struct xfs_mount		*mp = upd->mp;
	int				error;

	ASSERT(xfs_imeta_path_check(upd->path));

	*ipp = NULL;

	if (xfs_has_metadir(mp))
		error = xfs_imeta_dir_create(upd, mode);
	else
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

	if (xfs_has_metadir(upd->mp))
		error = xfs_imeta_dir_unlink(upd);
	else
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

	if (xfs_has_metadir(upd->mp))
		return xfs_imeta_dir_link(upd);
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
	if (xfs_has_metadir(tp->t_mountp))
		return xfs_imeta_dir_mount(tp);

	return 0;
}

/* Create a path to a file within the metadata directory tree. */
int
xfs_imeta_create_file_path(
	struct xfs_mount	*mp,
	unsigned int		nr_components,
	struct xfs_imeta_path	**pathp)
{
	struct xfs_imeta_path	*p;
	unsigned char		**components;

	p = kzalloc(sizeof(struct xfs_imeta_path), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	components = kvcalloc(nr_components, sizeof(unsigned char *),
			GFP_KERNEL);
	if (!components) {
		kfree(p);
		return -ENOMEM;
	}

	p->im_depth = nr_components;
	p->im_path = (const unsigned char **)components;
	p->im_ftype = XFS_DIR3_FT_REG_FILE;
	*pathp = p;
	return 0;
}

/* Free a metadata directory tree path. */
void
xfs_imeta_free_path(
	const struct xfs_imeta_path	*path)
{
	unsigned int			i;

	if (path->im_flags & XFS_IMETA_PATH_STATIC)
		return;

	for (i = 0; i < path->im_depth; i++) {
		if ((path->im_dynamicmask & (1ULL << i)) && path->im_path[i])
			kfree(path->im_path[i]);
	}
	kfree(path->im_path);
	kfree(path);
}

/*
 * Is the amount of space that could be allocated towards a given metadata
 * file at or beneath a certain threshold?
 */
static inline bool
xfs_imeta_resv_can_cover(
	struct xfs_inode	*ip,
	int64_t			rhs)
{
	/*
	 * The amount of space that can be allocated to this metadata file is
	 * the remaining reservation for the particular metadata file + the
	 * global free block count.  Take care of the first case to avoid
	 * touching the per-cpu counter.
	 */
	if (ip->i_delayed_blks >= rhs)
		return true;

	/*
	 * There aren't enough blocks left in the inode's reservation, but it
	 * isn't critical unless there also isn't enough free space.
	 */
	return __percpu_counter_compare(&ip->i_mount->m_fdblocks,
			rhs - ip->i_delayed_blks, 2048) >= 0;
}

/*
 * Is this metadata file critically low on blocks?  For now we'll define that
 * as the number of blocks we can get our hands on being less than 10% of what
 * we reserved or less than some arbitrary number (maximum btree height).
 */
bool
xfs_imeta_resv_critical(
	struct xfs_inode	*ip)
{
	uint64_t		asked_low_water;

	if (!ip)
		return false;

	ASSERT(xfs_is_metadir_inode(ip));
	trace_xfs_imeta_resv_critical(ip, 0);

	if (!xfs_imeta_resv_can_cover(ip, ip->i_mount->m_rtbtree_maxlevels))
		return true;

	asked_low_water = div_u64(ip->i_meta_resv_asked, 10);
	if (!xfs_imeta_resv_can_cover(ip, asked_low_water))
		return true;

	return XFS_TEST_ERROR(false, ip->i_mount,
			XFS_ERRTAG_IMETA_RESV_CRITICAL);
}

/* Allocate a block from the metadata file's reservation. */
void
xfs_imeta_resv_alloc_extent(
	struct xfs_inode	*ip,
	struct xfs_alloc_arg	*args)
{
	int64_t			len = args->len;

	ASSERT(xfs_is_metadir_inode(ip));
	ASSERT(XFS_IS_DQDETACHED(ip->i_mount, ip));
	ASSERT(args->resv == XFS_AG_RESV_IMETA);

	trace_xfs_imeta_resv_alloc_extent(ip, args->len);

	/*
	 * Allocate the blocks from the metadata inode's block reservation
	 * and update the ondisk sb counter.
	 */
	if (ip->i_delayed_blks > 0) {
		int64_t		from_resv;

		from_resv = min_t(int64_t, len, ip->i_delayed_blks);
		ip->i_delayed_blks -= from_resv;
		xfs_mod_delalloc(ip->i_mount, -from_resv);
		xfs_trans_mod_sb(args->tp, XFS_TRANS_SB_RES_FDBLOCKS,
				-from_resv);
		len -= from_resv;
	}

	/*
	 * Any allocation in excess of the reservation requires in-core and
	 * on-disk fdblocks updates.
	 */
	if (len)
		xfs_trans_mod_sb(args->tp, XFS_TRANS_SB_FDBLOCKS, -len);

	ip->i_nblocks += args->len;
	xfs_trans_log_inode(args->tp, ip, XFS_ILOG_CORE);
}

/* Free a block to the metadata file's reservation. */
void
xfs_imeta_resv_free_extent(
	struct xfs_inode	*ip,
	struct xfs_trans	*tp,
	xfs_filblks_t		len)
{
	int64_t			to_resv;

	ASSERT(xfs_is_metadir_inode(ip));
	ASSERT(XFS_IS_DQDETACHED(ip->i_mount, ip));
	trace_xfs_imeta_resv_free_extent(ip, len);

	ip->i_nblocks -= len;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	/*
	 * Add the freed blocks back into the inode's delalloc reservation
	 * until it reaches the maximum size.  Update the ondisk fdblocks only.
	 */
	to_resv = ip->i_meta_resv_asked - (ip->i_nblocks + ip->i_delayed_blks);
	if (to_resv > 0) {
		to_resv = min_t(int64_t, to_resv, len);
		ip->i_delayed_blks += to_resv;
		xfs_mod_delalloc(ip->i_mount, to_resv);
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_RES_FDBLOCKS, to_resv);
		len -= to_resv;
	}

	/*
	 * Everything else goes back to the filesystem, so update the in-core
	 * and on-disk counters.
	 */
	if (len)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_FDBLOCKS, len);
}

/* Release a metadata file's space reservation. */
void
xfs_imeta_resv_free_inode(
	struct xfs_inode	*ip)
{
	if (!ip)
		return;

	ASSERT(xfs_is_metadir_inode(ip));
	trace_xfs_imeta_resv_free(ip, 0);

	xfs_mod_delalloc(ip->i_mount, -ip->i_delayed_blks);
	xfs_mod_fdblocks(ip->i_mount, ip->i_delayed_blks, true);
	ip->i_delayed_blks = 0;
	ip->i_meta_resv_asked = 0;
}

/* Set up a metadata file's space reservation. */
int
xfs_imeta_resv_init_inode(
	struct xfs_inode	*ip,
	xfs_filblks_t		ask)
{
	xfs_filblks_t		hidden_space;
	xfs_filblks_t		used;
	int			error;

	if (!ip || ip->i_meta_resv_asked > 0)
		return 0;

	ASSERT(xfs_is_metadir_inode(ip));

	/*
	 * Space taken by all other metadata btrees are accounted on-disk as
	 * used space.  We therefore only hide the space that is reserved but
	 * not used by the trees.
	 */
	used = ip->i_nblocks;
	if (used > ask)
		ask = used;
	hidden_space = ask - used;

	error = xfs_mod_fdblocks(ip->i_mount, -(int64_t)hidden_space, true);
	if (error) {
		trace_xfs_imeta_resv_init_error(ip, error, _RET_IP_);
		return error;
	}

	xfs_mod_delalloc(ip->i_mount, hidden_space);
	ip->i_delayed_blks = hidden_space;
	ip->i_meta_resv_asked = ask;

	trace_xfs_imeta_resv_init(ip, ask);
	return 0;
}
