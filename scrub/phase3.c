// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "libfrog/ptvar.h"
#include "xfs_scrub.h"
#include "common.h"
#include "counter.h"
#include "inodes.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"

/* Phase 3: Scan all inodes. */

struct scrub_inode_ctx {
	struct scrub_ctx	*ctx;

	/* Number of inodes scanned. */
	struct ptcounter	*icount;

	/* Per-thread lists of file repair items. */
	struct ptvar		*repair_ptlists;

	/* Set to true to abort all threads. */
	bool			aborted;

	/* Set to true if we want to defer file repairs to phase 4. */
	bool			always_defer_repairs;
};

/* Report a filesystem error that the vfs fed us on close. */
static void
report_close_error(
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

/* Defer all the repairs until phase 4. */
static int
defer_inode_repair(
	struct scrub_inode_ctx		*ictx,
	const struct scrub_item		*sri)
{
	struct action_list		*alist;
	struct action_item		*aitem = NULL;
	int				ret;

	ret = repair_item_to_action_item(ictx->ctx, sri, &aitem);
	if (ret || !aitem)
		return ret;

	alist = ptvar_get(ictx->repair_ptlists, &ret);
	if (ret) {
		str_liberror(ictx->ctx, ret,
 _("getting per-thread inode repair list"));
		return ret;
	}

	action_list_add(alist, aitem);
	return 0;
}

/* Run repair actions now and leave unfinished items for later. */
static int
try_inode_repair(
	struct scrub_inode_ctx		*ictx,
	struct scrub_item		*sri,
	int				fd)
{
	/*
	 * If at the start of phase 3 we already had ag/rt metadata repairs
	 * queued up for phase 4, leave the action list untouched so that file
	 * metadata repairs will be deferred until phase 4.
	 */
	if (ictx->always_defer_repairs)
		return 0;

	/*
	 * Try to repair the file metadata.  Unfixed metadata will remain in
	 * the scrub item state to be queued as a single action item.
	 */
	return repair_file_corruption(ictx->ctx, sri, fd);
}

/*
 * If we couldn't check all the scheduled file metadata items, try performing
 * spot repairs until we check everything or stop making forward progress.
 */
static int
repair_and_scrub_inode_loop(
	struct scrub_ctx	*ctx,
	struct xfs_bulkstat	*bstat,
	int			fd,
	struct scrub_item	*sri,
	bool			*defer)
{
	unsigned int		to_check;
	int			error;

	*defer = false;
	if (ctx->mode != SCRUB_MODE_REPAIR)
		return 0;

	to_check = scrub_item_count_needscheck(sri);
	while (to_check > 0) {
		unsigned int	nr;

		error = repair_file_corruption(ctx, sri, fd);
		if (error)
			return error;

		error = scrub_item_check_file(ctx, sri, fd);
		if (error)
			return error;

		nr = scrub_item_count_needscheck(sri);
		if (nr == to_check) {
			char	descr[DESCR_BUFSZ];

			/*
			 * We cannot make forward scanning progress with this
			 * inode, so defer the rest until phase 4.
			 */
			scrub_render_ino_descr(ctx, descr, DESCR_BUFSZ,
					bstat->bs_ino, bstat->bs_gen, NULL);
			str_info(ctx, descr,
 _("Unable to make forward checking progress; will try again in phase 4."));
			*defer = true;
			return 0;
		}
		to_check = nr;
	}

	return 0;
}

/* Verify the contents, xattrs, and extent maps of an inode. */
static int
scrub_inode(
	struct scrub_ctx	*ctx,
	struct xfs_handle	*handle,
	struct xfs_bulkstat	*bstat,
	void			*arg)
{
	struct scrub_item	sri;
	struct scrub_inode_ctx	*ictx = arg;
	struct ptcounter	*icount = ictx->icount;
	int			fd = -1;
	int			error;

	scrub_item_init_file(&sri, bstat);
	background_sleep();

	/*
	 * Open this regular file to pin it in memory.  Avoiding the use of
	 * scan-by-handle means that the in-kernel scrubber doesn't pay the
	 * cost of opening the handle (looking up the inode in the inode btree,
	 * grabbing the inode, checking the generation) with every scrub call.
	 *
	 * Ignore any runtime or corruption related errors here because we can
	 * fall back to scrubbing by handle.  ESTALE can be ignored for the
	 * following reasons:
	 *
	 *  - If the file has been deleted since bulkstat, there's nothing to
	 *    check.  Scrub-by-handle returns ENOENT for such inodes.
	 *  - If the file has been deleted and reallocated since bulkstat,
	 *    its ondisk metadata have been rewritten and is assumed to be ok.
	 *    Scrub-by-handle also returns ENOENT if the generation doesn't
	 *    match.
	 *  - The file itself is corrupt and cannot be loaded.  In this case,
	 *    we fall back to scrub-by-handle.
	 *
	 * Note: We cannot use this same trick for directories because the VFS
	 * will try to reconnect directory file handles to the root directory
	 * by walking '..' entries upwards, and loops in the dirent index
	 * btree will cause livelocks.
	 */
	if (S_ISREG(bstat->bs_mode))
		fd = scrub_open_handle(handle);

	/* Scrub the inode. */
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_INODE);

	/* Scrub all block mappings. */
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTD);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTA);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_BMBTC);

	/*
	 * Check file data contents, e.g. symlink and directory entries.
	 *
	 * Note: bs_mode==0 occurs when inumbers says an inode is allocated,
	 * bulkstat skips the inode, and bulkstat_single errors out when
	 * loading the inode.  This could be due to racing with ifree, but it
	 * could be a corrupt inode.  Either way, schedule all the data fork
	 * content scrubbers.  Better to have them return -ENOENT than miss
	 * some coverage.
	 */
	if (S_ISLNK(bstat->bs_mode) || !bstat->bs_mode)
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_SYMLINK);
	if (S_ISDIR(bstat->bs_mode) || !bstat->bs_mode)
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_DIR);

	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_XATTR);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_PARENT);

	/*
	 * Try to check all of the metadata items that we just scheduled.  If
	 * we return with some types still needing a check and the space
	 * metadata isn't also in need of repairs, try repairing any damaged
	 * file metadata that we've found so far, and try checking the file
	 * again.  Worst case, defer the repairs and the checks to phase 4 if
	 * we can't make any progress on anything.
	 */
	error = scrub_item_check_file(ctx, &sri, fd);
	if (error)
		goto out;

	if (!ictx->always_defer_repairs) {
		bool	defer_repairs;

		error = repair_and_scrub_inode_loop(ctx, bstat, fd, &sri,
				&defer_repairs);
		if (error || defer_repairs)
			goto out;
	}

	/* Try to repair the file while it's open. */
	error = try_inode_repair(ictx, &sri, fd);
	if (error)
		goto out;

out:
	if (error)
		ictx->aborted = true;

	error = ptcounter_add(icount, 1);
	if (error) {
		str_liberror(ctx, error,
				_("incrementing scanned inode counter"));
		ictx->aborted = true;
	}
	progress_add(1);

	if (!error && !ictx->aborted)
		error = defer_inode_repair(ictx, &sri);

	if (fd >= 0) {
		int	err2;

		err2 = close(fd);
		if (err2) {
			report_close_error(ctx, bstat);
			ictx->aborted = true;
		}
	}

	if (!error && ictx->aborted)
		error = ECANCELED;
	return error;
}

/*
 * Collect all the inode repairs in the file repair list.  No need for locks
 * here, since we're single-threaded.
 */
static int
collect_repairs(
	struct ptvar		*ptv,
	void			*data,
	void			*foreach_arg)
{
	struct scrub_ctx	*ctx = foreach_arg;
	struct action_list	*alist = data;

	action_list_merge(ctx->file_repair_list, alist);
	return 0;
}

/* Initialize this per-thread file repair item list. */
static void
action_ptlist_init(
	void			*priv)
{
	struct action_list	*alist = priv;

	action_list_init(alist);
}

/* Verify all the inodes in a filesystem. */
int
phase3_func(
	struct scrub_ctx	*ctx)
{
	struct scrub_inode_ctx	ictx = { .ctx = ctx };
	uint64_t		val;
	xfs_agnumber_t		agno;
	int			err;

	err = -ptvar_alloc(scrub_nproc(ctx), sizeof(struct action_list),
			action_ptlist_init, &ictx.repair_ptlists);
	if (err) {
		str_liberror(ctx, err,
	_("creating per-thread file repair item lists"));
		return err;
	}

	err = ptcounter_alloc(scrub_nproc(ctx), &ictx.icount);
	if (err) {
		str_liberror(ctx, err, _("creating scanned inode counter"));
		goto out_ptvar;
	}

	/*
	 * If we already have ag/fs metadata to repair from previous phases,
	 * we would rather not try to repair file metadata until we've tried
	 * to repair the space metadata.
	 */
	for (agno = 0; agno < ctx->mnt.fsgeom.agcount; agno++) {
		if (!action_list_empty(ctx->fs_repair_list))
			ictx.always_defer_repairs = true;
	}

	err = scrub_scan_all_inodes(ctx, scrub_inode, &ictx);
	if (!err && ictx.aborted)
		err = ECANCELED;
	if (err)
		goto out_ptcounter;

	/*
	 * Combine all of the file repair items into the main repair list.
	 * We don't need locks here since we're the only thread running now.
	 */
	err = -ptvar_foreach(ictx.repair_ptlists, collect_repairs, ctx);
	if (err) {
		str_liberror(ctx, err, _("collecting inode repair lists"));
		goto out_ptcounter;
	}

	scrub_report_preen_triggers(ctx);
	err = ptcounter_value(ictx.icount, &val);
	if (err) {
		str_liberror(ctx, err, _("summing scanned inode counter"));
		goto out_ptcounter;
	}

	ctx->inodes_checked = val;
out_ptcounter:
	ptcounter_free(ictx.icount);
out_ptvar:
	ptvar_free(ictx.repair_ptlists);
	return err;
}

/* Estimate how much work we're going to do. */
int
phase3_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = ctx->mnt_sv.f_files - ctx->mnt_sv.f_ffree;
	*nr_threads = scrub_nproc(ctx);
	*rshift = 0;
	return 0;
}
