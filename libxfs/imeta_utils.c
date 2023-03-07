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
#include "xfs_da_format.h"
#include "xfs_trans_resv.h"
#include "xfs_trans_space.h"
#include "xfs_mount.h"
#include "xfs_trans.h"
#include "xfs_inode.h"
#include "xfs_da_btree.h"
#include "xfs_bmap_btree.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_imeta.h"
#include "xfs_trace.h"
#include "imeta_utils.h"

/* Initialize a metadata update structure. */
static inline int
xfs_imeta_init(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_imeta_update		*upd)
{
	memset(upd, 0, sizeof(struct xfs_imeta_update));
	upd->mp = mp;
	upd->path = path;
	return 0;
}

/*
 * Unlock and release resources after committing (or cancelling) a metadata
 * directory tree operation.  The caller retains its reference to @upd->ip
 * and must release it explicitly.
 */
static inline void
xfs_imeta_teardown(
	struct xfs_imeta_update		*upd,
	int				error)
{
	trace_xfs_imeta_teardown(upd, error);

	if (upd->ip) {
		if (upd->ip_locked)
			xfs_iunlock(upd->ip, XFS_ILOCK_EXCL);
		upd->ip_locked = false;
	}
}

/*
 * Begin the process of creating a metadata file by allocating transactions
 * and taking whatever resources we're going to need.
 */
int
xfs_imeta_start_create(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_imeta_update		*upd)
{
	int				error;

	error = xfs_imeta_init(mp, path, upd);
	if (error)
		return error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_create,
			xfs_create_space_res(mp, MAXNAMELEN), 0, 0, &upd->tp);
	if (error)
		goto out_teardown;

	trace_xfs_imeta_start_create(upd);
	return 0;
out_teardown:
	xfs_imeta_teardown(upd, error);
	return error;
}

/*
 * Begin the process of linking a metadata file by allocating transactions
 * and locking whatever resources we're going to need.
 */
static inline int
xfs_imeta_start_dir_update(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		*ip,
	struct xfs_trans_res		*tr_resv,
	unsigned int			resblks,
	struct xfs_imeta_update		*upd)
{
	int				error;

	error = xfs_imeta_init(mp, path, upd);
	if (error)
		return error;

	upd->ip = ip;

	error = xfs_trans_alloc_inode(upd->ip, tr_resv, resblks, 0, false,
			&upd->tp);
	if (error)
		goto out_teardown;

	upd->ip_locked = true;
	return 0;
out_teardown:
	xfs_imeta_teardown(upd, error);
	return error;
}

/*
 * Begin the process of linking a metadata file by allocating transactions
 * and locking whatever resources we're going to need.
 */
int
xfs_imeta_start_link(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		*ip,
	struct xfs_imeta_update		*upd)
{
	int				error;

	error = xfs_imeta_start_dir_update(mp, path, ip, &M_RES(mp)->tr_link,
			xfs_link_space_res(mp, MAXNAMELEN), upd);
	if (error)
		return error;

	trace_xfs_imeta_start_link(upd);
	return 0;
}

/*
 * Begin the process of unlinking a metadata file by allocating transactions
 * and locking whatever resources we're going to need.
 */
int
xfs_imeta_start_unlink(
	struct xfs_mount		*mp,
	const struct xfs_imeta_path	*path,
	struct xfs_inode		*ip,
	struct xfs_imeta_update		*upd)
{
	int				error;

	error = xfs_imeta_start_dir_update(mp, path, ip, &M_RES(mp)->tr_remove,
			xfs_remove_space_res(mp, MAXNAMELEN), upd);
	if (error)
		return error;

	trace_xfs_imeta_start_unlink(upd);
	return 0;
}

/* Commit a metadir update and unlock/drop all resources. */
int
xfs_imeta_commit_update(
	struct xfs_imeta_update		*upd)
{
	int				error;

	trace_xfs_imeta_update_commit(upd);

	error = xfs_trans_commit(upd->tp);
	upd->tp = NULL;

	xfs_imeta_teardown(upd, error);
	return error;
}

/* Cancel a metadir update and unlock/drop all resources. */
void
xfs_imeta_cancel_update(
	struct xfs_imeta_update		*upd,
	int				error)
{
	trace_xfs_imeta_update_cancel(upd);

	xfs_trans_cancel(upd->tp);
	upd->tp = NULL;

	xfs_imeta_teardown(upd, error);
}
