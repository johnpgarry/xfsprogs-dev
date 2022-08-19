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

/* Scrub each AG's metadata btrees. */
static void
scan_ag_metadata(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*arg)
{
	struct scrub_ctx		*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct scan_ctl			*sctl = arg;
	struct action_list		alist;
	struct action_list		immediate_alist;
	unsigned long long		broken_primaries;
	unsigned long long		broken_secondaries;
	char				descr[DESCR_BUFSZ];
	int				ret;

	if (sctl->aborted)
		return;

	action_list_init(&alist);
	action_list_init(&immediate_alist);
	snprintf(descr, DESCR_BUFSZ, _("AG %u"), agno);

	/*
	 * First we scrub and fix the AG headers, because we need
	 * them to work well enough to check the AG btrees.
	 */
	ret = scrub_ag_headers(ctx, agno, &alist);
	if (ret)
		goto err;

	/* Repair header damage. */
	ret = action_list_process_or_defer(ctx, agno, &alist);
	if (ret)
		goto err;

	/* Now scrub the AG btrees. */
	ret = scrub_ag_metadata(ctx, agno, &alist);
	if (ret)
		goto err;

	/*
	 * Figure out if we need to perform early fixing.  The only
	 * reason we need to do this is if the inobt is broken, which
	 * prevents phase 3 (inode scan) from running.  We can rebuild
	 * the inobt from rmapbt data, but if the rmapbt is broken even
	 * at this early phase then we are sunk.
	 */
	broken_secondaries = 0;
	broken_primaries = 0;
	action_list_find_mustfix(&alist, &immediate_alist,
			&broken_primaries, &broken_secondaries);
	if (broken_secondaries && !debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
		if (broken_primaries)
			str_info(ctx, descr,
_("Corrupt primary and secondary block mapping metadata."));
		else
			str_info(ctx, descr,
_("Corrupt secondary block mapping metadata."));
		str_info(ctx, descr,
_("Filesystem might not be repairable."));
	}

	/* Repair (inode) btree damage. */
	ret = action_list_process_or_defer(ctx, agno, &immediate_alist);
	if (ret)
		goto err;

	/* Everything else gets fixed during phase 4. */
	action_list_defer(ctx, agno, &alist);
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
	struct action_list	alist;
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct scan_ctl		*sctl = arg;
	int			ret;

	if (sctl->aborted)
		goto out;

	action_list_init(&alist);
	ret = scrub_fs_metadata(ctx, type, &alist);
	if (ret) {
		sctl->aborted = true;
		goto out;
	}

	action_list_defer(ctx, 0, &alist);

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
	struct action_list	alist;
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
	 * In case we ever use the primary super scrubber to perform fs
	 * upgrades (followed by a full scrub), do that before we launch
	 * anything else.
	 */
	action_list_init(&alist);
	ret = scrub_primary_super(ctx, &alist);
	if (ret)
		goto out_wq;
	ret = action_list_process_or_defer(ctx, 0, &alist);
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
