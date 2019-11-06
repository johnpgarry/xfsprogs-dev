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
static int
scrub_fd(
	struct scrub_ctx	*ctx,
	int			(*fn)(struct scrub_ctx *ctx, uint64_t ino,
				      uint32_t gen, struct action_list *a),
	struct xfs_bulkstat	*bs,
	struct action_list	*alist)
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
	int			old_errno = errno;

	scrub_render_ino_descr(ctx, descr, DESCR_BUFSZ, bstat->bs_ino,
			bstat->bs_gen, NULL);
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
	struct action_list	alist;
	struct scrub_inode_ctx	*ictx = arg;
	struct ptcounter	*icount = ictx->icount;
	xfs_agnumber_t		agno;
	bool			moveon = true;
	int			fd = -1;
	int			error;

	action_list_init(&alist);
	agno = cvt_ino_to_agno(&ctx->mnt, bstat->bs_ino);
	background_sleep();

	/* Try to open the inode to pin it. */
	if (S_ISREG(bstat->bs_mode)) {
		fd = scrub_open_handle(handle);
		/* Stale inode means we scan the whole cluster again. */
		if (fd < 0 && errno == ESTALE)
			return ESTALE;
	}

	/* Scrub the inode. */
	error = scrub_fd(ctx, xfs_scrub_inode_fields, bstat, &alist);
	if (error)
		goto out;

	error = action_list_process_or_defer(ctx, agno, &alist);
	if (error)
		goto out;

	/* Scrub all block mappings. */
	error = scrub_fd(ctx, xfs_scrub_data_fork, bstat, &alist);
	if (error)
		goto out;
	error = scrub_fd(ctx, xfs_scrub_attr_fork, bstat, &alist);
	if (error)
		goto out;
	error = scrub_fd(ctx, xfs_scrub_cow_fork, bstat, &alist);
	if (error)
		goto out;

	error = action_list_process_or_defer(ctx, agno, &alist);
	if (error)
		goto out;

	if (S_ISLNK(bstat->bs_mode)) {
		/* Check symlink contents. */
		error = xfs_scrub_symlink(ctx, bstat->bs_ino, bstat->bs_gen,
				&alist);
	} else if (S_ISDIR(bstat->bs_mode)) {
		/* Check the directory entries. */
		error = scrub_fd(ctx, xfs_scrub_dir, bstat, &alist);
	}
	if (error)
		goto out;

	/* Check all the extended attributes. */
	error = scrub_fd(ctx, xfs_scrub_attr, bstat, &alist);
	if (error)
		goto out;

	/* Check parent pointers. */
	error = scrub_fd(ctx, xfs_scrub_parent, bstat, &alist);
	if (error)
		goto out;

	/* Try to repair the file while it's open. */
	error = action_list_process_or_defer(ctx, agno, &alist);
	if (error)
		goto out;

out:
	if (error)
		moveon = false;
	error = ptcounter_add(icount, 1);
	if (error) {
		str_liberror(ctx, error,
				_("incrementing scanned inode counter"));
		return false;
	}
	progress_add(1);
	action_list_defer(ctx, agno, &alist);
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
	uint64_t		val;
	int			err;

	ictx.moveon = true;
	err = ptcounter_alloc(scrub_nproc(ctx), &ictx.icount);
	if (err) {
		str_liberror(ctx, err, _("creating scanned inode counter"));
		return false;
	}

	err = scrub_scan_all_inodes(ctx, xfs_scrub_inode, &ictx);
	if (err)
		ictx.moveon = false;
	if (!ictx.moveon)
		goto free;
	xfs_scrub_report_preen_triggers(ctx);
	err = ptcounter_value(ictx.icount, &val);
	if (err) {
		str_liberror(ctx, err, _("summing scanned inode counter"));
		return false;
	}
	ctx->inodes_checked = val;
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
