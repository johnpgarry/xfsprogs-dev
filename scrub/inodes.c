// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include "platform_defs.h"
#include "xfs_arch.h"
#include "xfs_format.h"
#include "handle.h"
#include "path.h"
#include "workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "inodes.h"
#include "xfrog.h"

/*
 * Iterate a range of inodes.
 *
 * This is a little more involved than repeatedly asking BULKSTAT for a
 * buffer's worth of stat data for some number of inodes.  We want to scan as
 * many of the inodes that the inobt thinks there are, including the ones that
 * are broken, but if we ask for n inodes starting at x, it'll skip the bad
 * ones and fill from beyond the range (x + n).
 *
 * Therefore, we ask INUMBERS to return one inobt chunk's worth of inode
 * bitmap information.  Then we try to BULKSTAT only the inodes that were
 * present in that chunk, and compare what we got against what INUMBERS said
 * was there.  If there's a mismatch, we know that we have an inode that fails
 * the verifiers but we can inject the bulkstat information to force the scrub
 * code to deal with the broken inodes.
 *
 * If the iteration function returns ESTALE, that means that the inode has
 * been deleted and possibly recreated since the BULKSTAT call.  We wil
 * refresh the stat information and try again up to 30 times before reporting
 * the staleness as an error.
 */

/*
 * Did we get exactly the inodes we expected?  If not, load them one at a
 * time (or fake it) into the bulkstat data.
 */
static void
xfs_iterate_inodes_range_check(
	struct scrub_ctx	*ctx,
	struct xfs_inogrp	*inogrp,
	struct xfs_bstat	*bstat)
{
	struct xfs_bstat	*bs;
	int			i;
	int			error;

	for (i = 0, bs = bstat; i < XFS_INODES_PER_CHUNK; i++) {
		if (!(inogrp->xi_allocmask & (1ULL << i)))
			continue;
		if (bs->bs_ino == inogrp->xi_startino + i) {
			bs++;
			continue;
		}

		/* Load the one inode. */
		error = xfrog_bulkstat_single(&ctx->mnt,
				inogrp->xi_startino + i, bs);
		if (error || bs->bs_ino != inogrp->xi_startino + i) {
			memset(bs, 0, sizeof(struct xfs_bstat));
			bs->bs_ino = inogrp->xi_startino + i;
			bs->bs_blksize = ctx->mnt_sv.f_frsize;
		}
		bs++;
	}
}

/*
 * Call into the filesystem for inode/bulkstat information and call our
 * iterator function.  We'll try to fill the bulkstat information in batches,
 * but we also can detect iget failures.
 */
static bool
xfs_iterate_inodes_range(
	struct scrub_ctx	*ctx,
	const char		*descr,
	void			*fshandle,
	uint64_t		first_ino,
	uint64_t		last_ino,
	xfs_inode_iter_fn	fn,
	void			*arg)
{
	struct xfs_handle	handle;
	struct xfs_inogrp	inogrp;
	struct xfs_bstat	bstat[XFS_INODES_PER_CHUNK];
	char			idescr[DESCR_BUFSZ];
	char			buf[DESCR_BUFSZ];
	struct xfs_bstat	*bs;
	uint64_t		igrp_ino;
	uint64_t		ino;
	uint32_t		bulklen = 0;
	uint32_t		igrplen = 0;
	bool			moveon = true;
	int			i;
	int			error;
	int			stale_count = 0;


	memset(bstat, 0, XFS_INODES_PER_CHUNK * sizeof(struct xfs_bstat));

	memcpy(&handle.ha_fsid, fshandle, sizeof(handle.ha_fsid));
	handle.ha_fid.fid_len = sizeof(xfs_fid_t) -
			sizeof(handle.ha_fid.fid_len);
	handle.ha_fid.fid_pad = 0;

	/* Find the inode chunk & alloc mask */
	igrp_ino = first_ino;
	error = xfrog_inumbers(&ctx->mnt, &igrp_ino, 1, &inogrp, &igrplen);
	while (!error && igrplen) {
		/* Load the inodes. */
		ino = inogrp.xi_startino - 1;

		/*
		 * We can have totally empty inode chunks on filesystems where
		 * there are more than 64 inodes per block.  Skip these.
		 */
		if (inogrp.xi_alloccount == 0)
			goto igrp_retry;
		error = xfrog_bulkstat(&ctx->mnt, &ino, inogrp.xi_alloccount,
				bstat, &bulklen);
		if (error)
			str_info(ctx, descr, "%s", strerror_r(errno,
						buf, DESCR_BUFSZ));

		xfs_iterate_inodes_range_check(ctx, &inogrp, bstat);

		/* Iterate all the inodes. */
		for (i = 0, bs = bstat; i < inogrp.xi_alloccount; i++, bs++) {
			if (bs->bs_ino > last_ino)
				goto out;

			handle.ha_fid.fid_ino = bs->bs_ino;
			handle.ha_fid.fid_gen = bs->bs_gen;
			error = fn(ctx, &handle, bs, arg);
			switch (error) {
			case 0:
				break;
			case ESTALE:
				stale_count++;
				if (stale_count < 30) {
					igrp_ino = inogrp.xi_startino;
					goto igrp_retry;
				}
				xfs_scrub_render_ino(ctx, idescr, DESCR_BUFSZ,
						bs->bs_ino, bs->bs_gen);
				str_info(ctx, idescr,
_("Changed too many times during scan; giving up."));
				break;
			case XFS_ITERATE_INODES_ABORT:
				error = 0;
				/* fall thru */
			default:
				moveon = false;
				errno = error;
				goto err;
			}
			if (xfs_scrub_excessive_errors(ctx)) {
				moveon = false;
				goto out;
			}
		}

		stale_count = 0;
igrp_retry:
		error = xfrog_inumbers(&ctx->mnt, &igrp_ino, 1, &inogrp,
				&igrplen);
	}

err:
	if (error) {
		str_errno(ctx, descr);
		moveon = false;
	}
out:
	return moveon;
}

/* BULKSTAT wrapper routines. */
struct xfs_scan_inodes {
	xfs_inode_iter_fn	fn;
	void			*arg;
	bool			moveon;
};

/* Scan all the inodes in an AG. */
static void
xfs_scan_ag_inodes(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct xfs_scan_inodes	*si = arg;
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	char			descr[DESCR_BUFSZ];
	uint64_t		ag_ino;
	uint64_t		next_ag_ino;
	bool			moveon;

	snprintf(descr, DESCR_BUFSZ, _("dev %d:%d AG %u inodes"),
				major(ctx->fsinfo.fs_datadev),
				minor(ctx->fsinfo.fs_datadev),
				agno);

	ag_ino = xfrog_agino_to_ino(&ctx->mnt, agno, 0);
	next_ag_ino = xfrog_agino_to_ino(&ctx->mnt, agno + 1, 0);

	moveon = xfs_iterate_inodes_range(ctx, descr, ctx->fshandle, ag_ino,
			next_ag_ino - 1, si->fn, si->arg);
	if (!moveon)
		si->moveon = false;
}

/* Scan all the inodes in a filesystem. */
bool
xfs_scan_all_inodes(
	struct scrub_ctx	*ctx,
	xfs_inode_iter_fn	fn,
	void			*arg)
{
	struct xfs_scan_inodes	si;
	xfs_agnumber_t		agno;
	struct workqueue	wq;
	int			ret;

	si.moveon = true;
	si.fn = fn;
	si.arg = arg;

	ret = workqueue_create(&wq, (struct xfs_mount *)ctx,
			scrub_nproc_workqueue(ctx));
	if (ret) {
		str_liberror(ctx, ret, _("creating bulkstat workqueue"));
		return false;
	}

	for (agno = 0; agno < ctx->mnt.fsgeom.agcount; agno++) {
		ret = workqueue_add(&wq, xfs_scan_ag_inodes, agno, &si);
		if (ret) {
			si.moveon = false;
			str_liberror(ctx, ret, _("queueing bulkstat work"));
			break;
		}
	}

	ret = workqueue_terminate(&wq);
	if (ret) {
		si.moveon = false;
		str_liberror(ctx, ret, _("finishing bulkstat work"));
	}
	workqueue_destroy(&wq);

	return si.moveon;
}

/*
 * Open a file by handle, or return a negative error code.
 */
int
xfs_open_handle(
	struct xfs_handle	*handle)
{
	return open_by_fshandle(handle, sizeof(*handle),
			O_RDONLY | O_NOATIME | O_NOFOLLOW | O_NOCTTY);
}
