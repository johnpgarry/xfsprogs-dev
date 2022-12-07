// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"
#include "vfs.h"
#include "atomic.h"

/* Phase 4: Repair filesystem. */

struct repair_list_schedule {
	struct action_list		*repair_list;

	/* Action items that we could not resolve and want to try again. */
	struct action_list		requeue_list;

	pthread_mutex_t			lock;

	/* Workers use this to signal the scheduler when all work is done. */
	pthread_cond_t			done;

	/* Number of workers that are still running. */
	unsigned int			workers;

	/* Or should we all abort? */
	bool				aborted;

	/* Did we make any progress this round? */
	bool				made_progress;
};

/* Try to repair as many things on our list as we can. */
static void
repair_list_worker(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*priv)
{
	struct repair_list_schedule	*rls = priv;
	struct scrub_ctx		*ctx = (struct scrub_ctx *)wq->wq_ctx;

	pthread_mutex_lock(&rls->lock);
	while (!rls->aborted) {
		struct action_item	*aitem;
		enum tryrepair_outcome	outcome;
		int			ret;

		aitem = action_list_pop(rls->repair_list);
		if (!aitem)
			break;

		pthread_mutex_unlock(&rls->lock);
		ret = action_item_try_repair(ctx, aitem, &outcome);
		pthread_mutex_lock(&rls->lock);

		if (ret) {
			rls->aborted = true;
			free(aitem);
			break;
		}

		switch (outcome) {
		case TR_REQUEUE:
			/*
			 * Partial progress.  Make a note of that and requeue
			 * this item for the next round.
			 */
			rls->made_progress = true;
			action_list_add(&rls->requeue_list, aitem);
			break;
		case TR_NOPROGRESS:
			/*
			 * No progress.  Requeue this item for a later round,
			 * which could happen if something else makes progress.
			 */
			action_list_add(&rls->requeue_list, aitem);
			break;
		case TR_REPAIRED:
			/*
			 * All repairs for this item completed.  Free the item,
			 * and remember that progress was made.
			 */
			rls->made_progress = true;
			free(aitem);
			break;
		}
	}

	rls->workers--;
	if (rls->workers == 0)
		pthread_cond_broadcast(&rls->done);
	pthread_mutex_unlock(&rls->lock);
}

/*
 * Schedule repair list workers.  Returns 1 if we made progress, 0 if we
 * did not, or -1 if we need to abort everything.
 */
static int
repair_list_schedule(
	struct scrub_ctx		*ctx,
	struct workqueue		*wq,
	struct action_list		*repair_list)
{
	struct repair_list_schedule	rls = {
		.lock			= PTHREAD_MUTEX_INITIALIZER,
		.done			= PTHREAD_COND_INITIALIZER,
		.repair_list		= repair_list,
	};
	unsigned int			i;
	unsigned int			nr_workers = scrub_nproc(ctx);
	bool				made_any_progress = false;
	int				ret = 0;

	if (action_list_empty(repair_list))
		return 0;

	action_list_init(&rls.requeue_list);

	/*
	 * Use the workers to run through the entire repair list once.  Requeue
	 * anything that did not make progress, and keep trying as long as the
	 * workers made any kind of progress.
	 */
	do {
		rls.made_progress = false;

		/* Start all the worker threads. */
		for (i = 0; i < nr_workers; i++) {
			pthread_mutex_lock(&rls.lock);
			rls.workers++;
			pthread_mutex_unlock(&rls.lock);

			ret = -workqueue_add(wq, repair_list_worker, 0, &rls);
			if (ret) {
				str_liberror(ctx, ret,
 _("queueing repair list worker"));
				pthread_mutex_lock(&rls.lock);
				rls.workers--;
				pthread_mutex_unlock(&rls.lock);
				break;
			}
		}

		/* Wait for all worker functions to return. */
		pthread_mutex_lock(&rls.lock);
		while (rls.workers > 0)
			pthread_cond_wait(&rls.done, &rls.lock);
		pthread_mutex_unlock(&rls.lock);

		action_list_merge(repair_list, &rls.requeue_list);

		if (ret || rls.aborted)
			return -1;
		if (rls.made_progress)
			made_any_progress = true;
	} while (rls.made_progress && !action_list_empty(repair_list));

	if (made_any_progress)
	       return 1;
	return 0;
}

/* Process both repair lists. */
static int
repair_everything(
	struct scrub_ctx		*ctx)
{
	struct workqueue		wq;
	int				fixed_anything;
	int				ret;

	ret = -workqueue_create(&wq, (struct xfs_mount *)ctx,
			scrub_nproc_workqueue(ctx));
	if (ret) {
		str_liberror(ctx, ret, _("creating repair workqueue"));
		return ret;
	}

	/*
	 * Try to fix everything on the space metadata repair list and then the
	 * file repair list until we stop making progress.  These repairs can
	 * be threaded, if the user desires.
	 */
	do {
		fixed_anything = 0;

		ret = repair_list_schedule(ctx, &wq, ctx->fs_repair_list);
		if (ret < 0)
			break;
		if (ret == 1)
			fixed_anything++;

		ret = repair_list_schedule(ctx, &wq, ctx->file_repair_list);
		if (ret < 0)
			break;
		if (ret == 1)
			fixed_anything++;
	} while (fixed_anything > 0);

	ret = -workqueue_terminate(&wq);
	if (ret)
		str_liberror(ctx, ret, _("finishing repair work"));
	workqueue_destroy(&wq);

	if (ret < 0)
		return ret;

	/*
	 * Combine both repair lists and repair everything serially.  This is
	 * the last chance to fix things.
	 */
	action_list_merge(ctx->fs_repair_list, ctx->file_repair_list);
	return action_list_process(ctx, ctx->fs_repair_list, XRM_FINAL_WARNING);
}

/* Fix everything that needs fixing. */
int
phase4_func(
	struct scrub_ctx	*ctx)
{
	struct xfs_fsop_geom	fsgeom;
	struct scrub_item	sri;
	int			ret;

	if (action_list_empty(ctx->fs_repair_list) &&
	    action_list_empty(ctx->file_repair_list))
		return 0;

	if (ctx->mode == SCRUB_MODE_PREEN && ctx->corruptions_found) {
		str_info(ctx, ctx->mntpoint,
 _("Corruptions found; will not optimize.  Re-run without -p.\n"));
		return 0;
	}

	/*
	 * Check the resource usage counters early.  Normally we do this during
	 * phase 7, but some of the cross-referencing requires fairly accurate
	 * summary counters.  Check and try to repair them now to minimize the
	 * chance that repairs of primary metadata fail due to secondary
	 * metadata.  If repairs fails, we'll come back during phase 7.
	 */
	scrub_item_init_fs(&sri);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_FSCOUNTERS);

	/*
	 * Repair possibly bad quota counts before starting other repairs,
	 * because wildly incorrect quota counts can cause shutdowns.
	 * Quotacheck scans all inodes, so we only want to do it if we know
	 * it's sick.
	 */
	ret = xfrog_geometry(ctx->mnt.fd, &fsgeom);
	if (ret)
		return ret;

	if (fsgeom.sick & XFS_FSOP_GEOM_SICK_QUOTACHECK)
		scrub_item_schedule(&sri, XFS_SCRUB_TYPE_QUOTACHECK);

	/* Check and repair counters before starting on the rest. */
	ret = scrub_item_check(ctx, &sri);
	if (ret)
		return ret;
	ret = repair_item_corruption(ctx, &sri);
	if (ret)
		return ret;

	return repair_everything(ctx);
}

/* Estimate how much work we're going to do. */
int
phase4_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	unsigned long long	need_fixing;

	/* Everything on the repair lis. */
	need_fixing = action_list_length(ctx->fs_repair_list) +
		      action_list_length(ctx->file_repair_list);

	*items = need_fixing;
	*nr_threads = scrub_nproc(ctx) + 1;
	*rshift = 0;
	return 0;
}
