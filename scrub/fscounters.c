// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include "platform_defs.h"
#include "xfs_arch.h"
#include "xfs_format.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "fscounters.h"
#include "libfrog/bulkstat.h"

/*
 * Filesystem counter collection routines.  We can count the number of
 * inodes in the filesystem, and we can estimate the block counters.
 */

/* Count the number of inodes in the filesystem. */

/* INUMBERS wrapper routines. */
struct xfs_count_inodes {
	bool			moveon;
	uint64_t		counters[0];
};

/*
 * Count the number of inodes.  Use INUMBERS to figure out how many inodes
 * exist in the filesystem, assuming we've already scrubbed that.
 */
static bool
xfs_count_inodes_ag(
	struct scrub_ctx	*ctx,
	const char		*descr,
	uint32_t		agno,
	uint64_t		*count)
{
	struct xfs_inumbers_req	*ireq;
	uint64_t		nr = 0;
	unsigned int		i;
	int			error;

	ireq = xfrog_inumbers_alloc_req(64, 0);
	if (!ireq) {
		str_info(ctx, descr, _("Insufficient memory; giving up."));
		return false;
	}
	xfrog_inumbers_set_ag(ireq, agno);

	while (!(error = xfrog_inumbers(&ctx->mnt, ireq))) {
		if (ireq->hdr.ocount == 0)
			break;
		for (i = 0; i < ireq->hdr.ocount; i++)
			nr += ireq->inumbers[i].xi_alloccount;
	}

	free(ireq);

	if (error) {
		str_liberror(ctx, error, descr);
		return false;
	}

	*count = nr;
	return true;
}

/* Scan all the inodes in an AG. */
static void
xfs_count_ag_inodes(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct xfs_count_inodes	*ci = arg;
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	char			descr[DESCR_BUFSZ];
	bool			moveon;

	snprintf(descr, DESCR_BUFSZ, _("dev %d:%d AG %u inodes"),
				major(ctx->fsinfo.fs_datadev),
				minor(ctx->fsinfo.fs_datadev),
				agno);

	moveon = xfs_count_inodes_ag(ctx, descr, agno, &ci->counters[agno]);
	if (!moveon)
		ci->moveon = false;
}

/* Count all the inodes in a filesystem. */
bool
xfs_count_all_inodes(
	struct scrub_ctx	*ctx,
	uint64_t		*count)
{
	struct xfs_count_inodes	*ci;
	xfs_agnumber_t		agno;
	struct workqueue	wq;
	bool			moveon;
	int			ret;

	ci = calloc(1, sizeof(struct xfs_count_inodes) +
			(ctx->mnt.fsgeom.agcount * sizeof(uint64_t)));
	if (!ci)
		return false;
	ci->moveon = true;

	ret = workqueue_create(&wq, (struct xfs_mount *)ctx,
			scrub_nproc_workqueue(ctx));
	if (ret) {
		moveon = false;
		str_info(ctx, ctx->mntpoint, _("Could not create workqueue."));
		goto out_free;
	}
	for (agno = 0; agno < ctx->mnt.fsgeom.agcount; agno++) {
		ret = workqueue_add(&wq, xfs_count_ag_inodes, agno, ci);
		if (ret) {
			moveon = false;
			str_info(ctx, ctx->mntpoint,
_("Could not queue AG %u icount work."), agno);
			break;
		}
	}
	workqueue_destroy(&wq);

	for (agno = 0; agno < ctx->mnt.fsgeom.agcount; agno++)
		*count += ci->counters[agno];
	moveon = ci->moveon;

out_free:
	free(ci);
	return moveon;
}

/* Estimate the number of blocks and inodes in the filesystem. */
bool
xfs_scan_estimate_blocks(
	struct scrub_ctx		*ctx,
	unsigned long long		*d_blocks,
	unsigned long long		*d_bfree,
	unsigned long long		*r_blocks,
	unsigned long long		*r_bfree,
	unsigned long long		*f_files,
	unsigned long long		*f_free)
{
	struct xfs_fsop_counts		fc;
	struct xfs_fsop_resblks		rb;
	struct statvfs			sfs;
	int				error;

	/* Grab the fstatvfs counters, since it has to report accurately. */
	error = fstatvfs(ctx->mnt.fd, &sfs);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return false;
	}

	/* Fetch the filesystem counters. */
	error = ioctl(ctx->mnt.fd, XFS_IOC_FSCOUNTS, &fc);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return false;
	}

	/*
	 * XFS reserves some blocks to prevent hard ENOSPC, so add those
	 * blocks back to the free data counts.
	 */
	error = ioctl(ctx->mnt.fd, XFS_IOC_GET_RESBLKS, &rb);
	if (error)
		str_errno(ctx, ctx->mntpoint);
	sfs.f_bfree += rb.resblks_avail;

	*d_blocks = sfs.f_blocks;
	if (ctx->mnt.fsgeom.logstart > 0)
		*d_blocks += ctx->mnt.fsgeom.logblocks;
	*d_bfree = sfs.f_bfree;
	*r_blocks = ctx->mnt.fsgeom.rtblocks;
	*r_bfree = fc.freertx;
	*f_files = sfs.f_files;
	*f_free = sfs.f_ffree;

	return true;
}
