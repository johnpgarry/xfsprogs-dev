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
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "inodes.h"
#include "libfrog/fsgeom.h"
#include "libfrog/bulkstat.h"

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
	struct xfs_inumbers	*inumbers,
	struct xfs_bulkstat	*bstat)
{
	struct xfs_bulkstat	*bs;
	int			i;
	int			error;

	for (i = 0, bs = bstat; i < XFS_INODES_PER_CHUNK; i++) {
		if (!(inumbers->xi_allocmask & (1ULL << i)))
			continue;
		if (bs->bs_ino == inumbers->xi_startino + i) {
			bs++;
			continue;
		}

		/* Load the one inode. */
		error = xfrog_bulkstat_single(&ctx->mnt,
				inumbers->xi_startino + i, 0, bs);
		if (error || bs->bs_ino != inumbers->xi_startino + i) {
			memset(bs, 0, sizeof(struct xfs_bulkstat));
			bs->bs_ino = inumbers->xi_startino + i;
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
xfs_iterate_inodes_ag(
	struct scrub_ctx	*ctx,
	const char		*descr,
	void			*fshandle,
	uint32_t		agno,
	xfs_inode_iter_fn	fn,
	void			*arg)
{
	struct xfs_handle	handle;
	struct xfs_inumbers_req	*ireq;
	struct xfs_bulkstat_req	*breq;
	char			idescr[DESCR_BUFSZ];
	struct xfs_bulkstat	*bs;
	struct xfs_inumbers	*inumbers;
	bool			moveon = true;
	int			i;
	int			error;
	int			stale_count = 0;

	memcpy(&handle.ha_fsid, fshandle, sizeof(handle.ha_fsid));
	handle.ha_fid.fid_len = sizeof(xfs_fid_t) -
			sizeof(handle.ha_fid.fid_len);
	handle.ha_fid.fid_pad = 0;

	breq = xfrog_bulkstat_alloc_req(XFS_INODES_PER_CHUNK, 0);
	if (!breq) {
		str_info(ctx, descr, _("Insufficient memory; giving up."));
		return false;
	}

	ireq = xfrog_inumbers_alloc_req(1, 0);
	if (!ireq) {
		str_info(ctx, descr, _("Insufficient memory; giving up."));
		free(breq);
		return false;
	}
	inumbers = &ireq->inumbers[0];
	xfrog_inumbers_set_ag(ireq, agno);

	/* Find the inode chunk & alloc mask */
	error = xfrog_inumbers(&ctx->mnt, ireq);
	while (!error && ireq->hdr.ocount > 0) {
		/*
		 * We can have totally empty inode chunks on filesystems where
		 * there are more than 64 inodes per block.  Skip these.
		 */
		if (inumbers->xi_alloccount == 0)
			goto igrp_retry;

		breq->hdr.ino = inumbers->xi_startino;
		breq->hdr.icount = inumbers->xi_alloccount;
		error = xfrog_bulkstat(&ctx->mnt, breq);
		if (error) {
			char	errbuf[DESCR_BUFSZ];

			str_info(ctx, descr, "%s", strerror_r(error,
						errbuf, DESCR_BUFSZ));
		}

		xfs_iterate_inodes_range_check(ctx, inumbers, breq->bulkstat);

		/* Iterate all the inodes. */
		for (i = 0, bs = breq->bulkstat;
		     i < inumbers->xi_alloccount;
		     i++, bs++) {
			handle.ha_fid.fid_ino = bs->bs_ino;
			handle.ha_fid.fid_gen = bs->bs_gen;
			error = fn(ctx, &handle, bs, arg);
			switch (error) {
			case 0:
				break;
			case ESTALE:
				stale_count++;
				if (stale_count < 30) {
					ireq->hdr.ino = inumbers->xi_startino;
					goto igrp_retry;
				}
				snprintf(idescr, DESCR_BUFSZ, "inode %"PRIu64,
						(uint64_t)bs->bs_ino);
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
		error = xfrog_inumbers(&ctx->mnt, ireq);
	}

err:
	if (error) {
		str_liberror(ctx, error, descr);
		moveon = false;
	}
out:
	free(ireq);
	free(breq);
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
	bool			moveon;

	snprintf(descr, DESCR_BUFSZ, _("dev %d:%d AG %u inodes"),
				major(ctx->fsinfo.fs_datadev),
				minor(ctx->fsinfo.fs_datadev),
				agno);

	moveon = xfs_iterate_inodes_ag(ctx, descr, ctx->fshandle, agno,
			si->fn, si->arg);
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
