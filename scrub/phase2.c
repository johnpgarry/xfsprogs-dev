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
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "xfs_scrub.h"
#include "common.h"
#include "scrub.h"
#include "repair.h"

/* Phase 2: Check internal metadata. */

struct scan_ctl {
	/*
	 * Control mechanism to signal that the rt bitmap file scan is done and
	 * wake up any waiters.
	 */
	pthread_cond_t		rbm_wait;
	pthread_mutex_t		rbm_waitlock;
	bool			rbm_done;

	bool			aborted;
};

/* Warn about the types of mutual inconsistencies that may make repairs hard. */
static inline void
warn_repair_difficulties(
	struct scrub_ctx	*ctx,
	unsigned int		difficulty,
	const char		*descr)
{
	if (!(difficulty & REPAIR_DIFFICULTY_SECONDARY))
		return;
	if (debug_tweak_on("XFS_SCRUB_FORCE_REPAIR"))
		return;

	if (difficulty & REPAIR_DIFFICULTY_PRIMARY)
		str_info(ctx, descr, _("Corrupt primary and secondary metadata."));
	else
		str_info(ctx, descr, _("Corrupt secondary metadata."));
	str_info(ctx, descr, _("Filesystem might not be repairable."));
}

/* Add a scrub item that needs more work to fs metadata repair list. */
static int
defer_fs_repair(
	struct scrub_ctx	*ctx,
	const struct scrub_item	*sri)
{
	struct action_item	*aitem = NULL;
	int			error;

	error = repair_item_to_action_item(ctx, sri, &aitem);
	if (error || !aitem)
		return error;

	pthread_mutex_lock(&ctx->lock);
	action_list_add(ctx->fs_repair_list, aitem);
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

/*
 * If we couldn't check all the scheduled metadata items, try performing spot
 * repairs until we check everything or stop making forward progress.
 */
static int
repair_and_scrub_loop(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri,
	const char		*descr,
	bool			*defer)
{
	unsigned int		to_check;
	int			ret;

	*defer = false;
	if (ctx->mode != SCRUB_MODE_REPAIR)
		return 0;

	to_check = scrub_item_count_needscheck(sri);
	while (to_check > 0) {
		unsigned int	nr;

		ret = repair_item_corruption(ctx, sri);
		if (ret)
			return ret;

		ret = scrub_item_check(ctx, sri);
		if (ret)
			return ret;

		nr = scrub_item_count_needscheck(sri);
		if (nr == to_check) {
			/*
			 * We cannot make forward scanning progress with this
			 * metadata, so defer the rest until phase 4.
			 */
			str_info(ctx, descr,
 _("Unable to make forward checking progress; will try again in phase 4."));
			*defer = true;
			return 0;
		}
		to_check = nr;
	}

	return 0;
}

/* Scrub each AG's metadata btrees. */
static void
scan_ag_metadata(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*arg)
{
	struct scrub_item		sri;
	struct scrub_item		fix_now;
	struct scrub_ctx		*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct scan_ctl			*sctl = arg;
	char				descr[DESCR_BUFSZ];
	unsigned int			difficulty;
	bool				defer_repairs;
	int				ret;

	if (sctl->aborted)
		return;

	scrub_item_init_ag(&sri, agno);
	snprintf(descr, DESCR_BUFSZ, _("AG %u"), agno);

	/*
	 * First we scrub and fix the AG headers, because we need them to work
	 * well enough to check the AG btrees.  Then scrub the AG btrees.
	 */
	scrub_item_schedule_group(&sri, XFROG_SCRUB_GROUP_AGHEADER);
	scrub_item_schedule_group(&sri, XFROG_SCRUB_GROUP_PERAG);

	/*
	 * Try to check all of the AG metadata items that we just scheduled.
	 * If we return with some types still needing a check, try repairing
	 * any damaged metadata that we've found so far, and try again.  Abort
	 * if we stop making forward progress.
	 */
	ret = scrub_item_check(ctx, &sri);
	if (ret)
		goto err;

	ret = repair_and_scrub_loop(ctx, &sri, descr, &defer_repairs);
	if (ret)
		goto err;
	if (defer_repairs)
		goto defer;

	/*
	 * Figure out if we need to perform early fixing.  The only
	 * reason we need to do this is if the inobt is broken, which
	 * prevents phase 3 (inode scan) from running.  We can rebuild
	 * the inobt from rmapbt data, but if the rmapbt is broken even
	 * at this early phase then we are sunk.
	 */
	difficulty = repair_item_difficulty(&sri);
	repair_item_mustfix(&sri, &fix_now);
	warn_repair_difficulties(ctx, difficulty, descr);

	/* Repair (inode) btree damage. */
	ret = repair_item_corruption(ctx, &fix_now);
	if (ret)
		goto err;

defer:
	/* Everything else gets fixed during phase 4. */
	ret = defer_fs_repair(ctx, &sri);
	if (ret)
		goto err;
	return;
err:
	sctl->aborted = true;
}

/* Scan whole-fs metadata. */
static void
scan_fs_metadata(
	struct workqueue	*wq,
	xfs_agnumber_t		type,
	void			*arg)
{
	struct scrub_item	sri;
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct scan_ctl		*sctl = arg;
	unsigned int		difficulty;
	bool			defer_repairs;
	int			ret;

	if (sctl->aborted)
		goto out;

	/*
	 * Try to check all of the metadata files that we just scheduled.  If
	 * we return with some types still needing a check, try repairing any
	 * damaged metadata that we've found so far, and try again.  Abort if
	 * we stop making forward progress.
	 */
	scrub_item_init_fs(&sri);
	scrub_item_schedule(&sri, type);
	ret = scrub_item_check(ctx, &sri);
	if (ret) {
		sctl->aborted = true;
		goto out;
	}

	ret = repair_and_scrub_loop(ctx, &sri, xfrog_scrubbers[type].descr,
			&defer_repairs);
	if (ret) {
		sctl->aborted = true;
		goto out;
	}
	if (defer_repairs)
		goto defer;

	/* Complain about metadata corruptions that might not be fixable. */
	difficulty = repair_item_difficulty(&sri);
	warn_repair_difficulties(ctx, difficulty, xfrog_scrubbers[type].descr);

defer:
	ret = defer_fs_repair(ctx, &sri);
	if (ret) {
		sctl->aborted = true;
		goto out;
	}

out:
	if (type == XFS_SCRUB_TYPE_RTBITMAP) {
		pthread_mutex_lock(&sctl->rbm_waitlock);
		sctl->rbm_done = true;
		pthread_cond_broadcast(&sctl->rbm_wait);
		pthread_mutex_unlock(&sctl->rbm_waitlock);
	}
}

/* Scan all filesystem metadata. */
int
phase2_func(
	struct scrub_ctx	*ctx)
{
	struct workqueue	wq;
	struct scan_ctl		sctl = {
		.aborted	= false,
		.rbm_done	= false,
	};
	struct scrub_item	sri;
	const struct xfrog_scrub_descr *sc = xfrog_scrubbers;
	xfs_agnumber_t		agno;
	unsigned int		type;
	int			ret, ret2;

	pthread_mutex_init(&sctl.rbm_waitlock, NULL);
	pthread_cond_init(&sctl.rbm_wait, NULL);

	ret = -workqueue_create(&wq, (struct xfs_mount *)ctx,
			scrub_nproc_workqueue(ctx));
	if (ret) {
		str_liberror(ctx, ret, _("creating scrub workqueue"));
		goto out_wait;
	}

	/*
	 * Scrub primary superblock.  This will be useful if we ever need to
	 * hook a filesystem-wide pre-scrub activity (e.g. enable filesystem
	 * upgrades) off of the sb 0 scrubber (which currently does nothing).
	 * If errors occur, this function will log them and return nonzero.
	 */
	scrub_item_init_ag(&sri, 0);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_SB);
	ret = scrub_item_check(ctx, &sri);
	if (ret)
		goto out_wq;
	ret = repair_item_completely(ctx, &sri);
	if (ret)
		goto out_wq;

	/* Scan each AG in parallel. */
	for (agno = 0;
	     agno < ctx->mnt.fsgeom.agcount && !sctl.aborted;
	     agno++) {
		ret = -workqueue_add(&wq, scan_ag_metadata, agno, &sctl);
		if (ret) {
			str_liberror(ctx, ret, _("queueing per-AG scrub work"));
			goto out_wq;
		}
	}

	if (sctl.aborted)
		goto out_wq;

	/*
	 * Scan all of the whole-fs metadata objects: realtime bitmap, realtime
	 * summary, and the three quota files.  Each of the metadata files can
	 * be scanned in parallel except for the realtime summary file, which
	 * must run after the realtime bitmap has been scanned.
	 */
	for (type = 0; type < XFS_SCRUB_TYPE_NR; type++, sc++) {
		if (sc->group != XFROG_SCRUB_GROUP_FS)
			continue;
		if (type == XFS_SCRUB_TYPE_RTSUM)
			continue;

		ret = -workqueue_add(&wq, scan_fs_metadata, type, &sctl);
		if (ret) {
			str_liberror(ctx, ret,
	_("queueing whole-fs scrub work"));
			goto out_wq;
		}
	}

	if (sctl.aborted)
		goto out_wq;

	/*
	 * Wait for the rt bitmap to finish scanning, then scan the rt summary
	 * since the summary can be regenerated completely from the bitmap.
	 */
	pthread_mutex_lock(&sctl.rbm_waitlock);
	while (!sctl.rbm_done)
		pthread_cond_wait(&sctl.rbm_wait, &sctl.rbm_waitlock);
	pthread_mutex_unlock(&sctl.rbm_waitlock);

	if (sctl.aborted)
		goto out_wq;

	ret = -workqueue_add(&wq, scan_fs_metadata, XFS_SCRUB_TYPE_RTSUM, &sctl);
	if (ret) {
		str_liberror(ctx, ret, _("queueing rtsummary scrub work"));
		goto out_wq;
	}

out_wq:
	ret2 = -workqueue_terminate(&wq);
	if (ret2) {
		str_liberror(ctx, ret2, _("finishing scrub work"));
		if (!ret && ret2)
			ret = ret2;
	}
	workqueue_destroy(&wq);
out_wait:
	pthread_cond_destroy(&sctl.rbm_wait);
	pthread_mutex_destroy(&sctl.rbm_waitlock);

	if (!ret && sctl.aborted)
		ret = ECANCELED;
	return ret;
}

/* Estimate how much work we're going to do. */
int
phase2_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = scrub_estimate_ag_work(ctx);
	*nr_threads = scrub_nproc(ctx);
	*rshift = 0;
	return 0;
}
