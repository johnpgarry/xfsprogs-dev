// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "counter.h"
#include "inodes.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"

/* Phase 3: Scan all inodes. */

/*
 * Run a per-file metadata scanner.  We use the ino/gen interface to
 * ensure that the inode we're checking matches what the inode scan
 * told us to look at.
 */
static bool
xfs_scrub_fd(
	struct scrub_ctx	*ctx,
	bool			(*fn)(struct scrub_ctx *ctx, uint64_t ino,
				      uint32_t gen, struct xfs_action_list *a),
	struct xfs_bulkstat	*bs,
	struct xfs_action_list	*alist)
{
	return fn(ctx, bs->bs_ino, bs->bs_gen, alist);
}

struct scrub_inode_ctx {
	struct ptcounter	*icount;
	bool			moveon;
};

/* Report a filesystem error that the vfs fed us on close. */
static void
xfs_scrub_inode_vfs_error(
	struct scrub_ctx	*ctx,
	struct xfs_bulkstat	*bstat)
{
	char			descr[DESCR_BUFSZ];
	xfs_agnumber_t		agno;
	xfs_agino_t		agino;
	int			old_errno = errno;

	agno = cvt_ino_to_agno(&ctx->mnt, bstat->bs_ino);
	agino = cvt_ino_to_agino(&ctx->mnt, bstat->bs_ino);
	snprintf(descr, DESCR_BUFSZ, _("inode %"PRIu64" (%u/%u)"),
			(uint64_t)bstat->bs_ino, agno, agino);
	errno = old_errno;
	str_errno(ctx, descr);
}

/* Verify the contents, xattrs, and extent maps of an inode. */
static int
xfs_scrub_inode(
	struct scrub_ctx	*ctx,
	struct xfs_handle	*handle,
	struct xfs_bulkstat	*bstat,
	void			*arg)
{
	struct xfs_action_list	alist;
	struct scrub_inode_ctx	*ictx = arg;
	struct ptcounter	*icount = ictx->icount;
	xfs_agnumber_t		agno;
	bool			moveon = true;
	int			fd = -1;
	int			error;

	xfs_action_list_init(&alist);
	agno = cvt_ino_to_agno(&ctx->mnt, bstat->bs_ino);
	background_sleep();

	/* Try to open the inode to pin it. */
	if (S_ISREG(bstat->bs_mode)) {
		fd = xfs_open_handle(handle);
		/* Stale inode means we scan the whole cluster again. */
		if (fd < 0 && errno == ESTALE)
			return ESTALE;
	}

	/* Scrub the inode. */
	moveon = xfs_scrub_fd(ctx, xfs_scrub_inode_fields, bstat, &alist);
	if (!moveon)
		goto out;

	moveon = xfs_action_list_process_or_defer(ctx, agno, &alist);
	if (!moveon)
		goto out;

	/* Scrub all block mappings. */
	moveon = xfs_scrub_fd(ctx, xfs_scrub_data_fork, bstat, &alist);
	if (!moveon)
		goto out;
	moveon = xfs_scrub_fd(ctx, xfs_scrub_attr_fork, bstat, &alist);
	if (!moveon)
		goto out;
	moveon = xfs_scrub_fd(ctx, xfs_scrub_cow_fork, bstat, &alist);
	if (!moveon)
		goto out;

	moveon = xfs_action_list_process_or_defer(ctx, agno, &alist);
	if (!moveon)
		goto out;

	if (S_ISLNK(bstat->bs_mode)) {
		/* Check symlink contents. */
		moveon = xfs_scrub_symlink(ctx, bstat->bs_ino, bstat->bs_gen,
				&alist);
	} else if (S_ISDIR(bstat->bs_mode)) {
		/* Check the directory entries. */
		moveon = xfs_scrub_fd(ctx, xfs_scrub_dir, bstat, &alist);
	}
	if (!moveon)
		goto out;

	/* Check all the extended attributes. */
	moveon = xfs_scrub_fd(ctx, xfs_scrub_attr, bstat, &alist);
	if (!moveon)
		goto out;

	/* Check parent pointers. */
	moveon = xfs_scrub_fd(ctx, xfs_scrub_parent, bstat, &alist);
	if (!moveon)
		goto out;

	/* Try to repair the file while it's open. */
	moveon = xfs_action_list_process_or_defer(ctx, agno, &alist);
	if (!moveon)
		goto out;

out:
	ptcounter_add(icount, 1);
	progress_add(1);
	xfs_action_list_defer(ctx, agno, &alist);
	if (fd >= 0) {
		error = close(fd);
		if (error)
			xfs_scrub_inode_vfs_error(ctx, bstat);
	}
	if (!moveon)
		ictx->moveon = false;
	return ictx->moveon ? 0 : XFS_ITERATE_INODES_ABORT;
}

/* Verify all the inodes in a filesystem. */
bool
xfs_scan_inodes(
	struct scrub_ctx	*ctx)
{
	struct scrub_inode_ctx	ictx;
	bool			ret;

	ictx.moveon = true;
	ictx.icount = ptcounter_init(scrub_nproc(ctx));
	if (!ictx.icount) {
		str_info(ctx, ctx->mntpoint, _("Could not create counter."));
		return false;
	}

	ret = xfs_scan_all_inodes(ctx, xfs_scrub_inode, &ictx);
	if (!ret)
		ictx.moveon = false;
	if (!ictx.moveon)
		goto free;
	xfs_scrub_report_preen_triggers(ctx);
	ctx->inodes_checked = ptcounter_value(ictx.icount);

free:
	ptcounter_free(ictx.icount);
	return ictx.moveon;
}

/* Estimate how much work we're going to do. */
bool
xfs_estimate_inodes_work(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = ctx->mnt_sv.f_files - ctx->mnt_sv.f_ffree;
	*nr_threads = scrub_nproc(ctx);
	*rshift = 0;
	return true;
}
