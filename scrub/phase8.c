// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
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
#include "libfrog/histogram.h"
#include "xfs_scrub.h"
#include "common.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"
#include "vfs.h"
#include "atomic.h"

/* Phase 8: Trim filesystem. */

static inline bool
fstrim_ok(
	struct scrub_ctx	*ctx)
{
	/*
	 * If errors remain on the filesystem, do not trim anything.  We don't
	 * have any threads running, so it's ok to skip the ctx lock here.
	 */
	if (!action_list_empty(ctx->fs_repair_list))
		return false;
	if (!action_list_empty(ctx->file_repair_list))
		return false;

	if (ctx->corruptions_found != 0)
		return false;
	if (ctx->unfixable_errors != 0)
		return false;

	if (ctx->runtime_errors != 0)
		return false;

	return true;
}

/*
 * Limit the amount of fstrim scanning that we let the kernel do in a single
 * call so that we can implement decent progress reporting and CPU resource
 * control.  Pick a prime number of gigabytes for interest.
 */
#define FSTRIM_MAX_BYTES	(11ULL << 30)

/* Trim a certain range of the filesystem. */
static int
fstrim_fsblocks(
	struct scrub_ctx	*ctx,
	uint64_t		start_fsb,
	uint64_t		fsbcount,
	uint64_t		minlen_fsb,
	bool			ignore_einval)
{
	uint64_t		start = cvt_off_fsb_to_b(&ctx->mnt, start_fsb);
	uint64_t		len = cvt_off_fsb_to_b(&ctx->mnt, fsbcount);
	uint64_t		minlen = cvt_off_fsb_to_b(&ctx->mnt, minlen_fsb);
	int			error;

	while (len > 0) {
		uint64_t	run;

		run = min(len, FSTRIM_MAX_BYTES);

		error = fstrim(ctx, start, run, minlen);
		if (error == EINVAL && ignore_einval)
			error = EOPNOTSUPP;
		if (error == EOPNOTSUPP) {
			/* Pretend we finished all the work. */
			progress_add(len);
			return 0;
		}
		if (error) {
			char		descr[DESCR_BUFSZ];

			snprintf(descr, sizeof(descr) - 1,
					_("fstrim start 0x%llx run 0x%llx minlen 0x%llx"),
					(unsigned long long)start,
					(unsigned long long)run,
					(unsigned long long)minlen);
			str_liberror(ctx, error, descr);
			return error;
		}

		progress_add(run);
		len -= run;
		start += run;
	}

	return 0;
}

/* Compute a suitable minlen parameter for fstrim. */
static uint64_t
fstrim_compute_minlen(
	const struct scrub_ctx	*ctx,
	const struct histogram	*freesp_hist)
{
	struct histogram	cdf;
	uint64_t		ret = 0;
	double			blk_threshold = 0;
	unsigned int		i;
	unsigned int		ag_max_usable;
	int			error;

	/*
	 * The kernel will reject a minlen that's larger than m_ag_max_usable.
	 * We can't calculate or query that value directly, so we guesstimate
	 * that it's 95% of the AG size.
	 */
	ag_max_usable = ctx->mnt.fsgeom.agblocks * 95 / 100;

	if (freesp_hist->totexts == 0)
		goto out;

	if (debug > 1)
		hist_print(freesp_hist);

	/* Insufficient samples to make a meaningful histogram */
	if (freesp_hist->totexts < freesp_hist->nr_buckets * 10)
		goto out;

	hist_init(&cdf);
	error = hist_cdf(freesp_hist, &cdf);
	if (error)
		goto out_free;

	blk_threshold = freesp_hist->totblocks * ctx->fstrim_block_pct;
	for (i = 1; i < freesp_hist->nr_buckets; i++) {
		if (cdf.buckets[i].blocks < blk_threshold) {
			ret = freesp_hist->buckets[i - 1].low;
			break;
		}
	}

out_free:
	hist_free(&cdf);
out:
	if (debug > 1)
		printf(_("fstrim minlen %lld threshold %lld ag_max_usable %u\n"),
				(unsigned long long)ret,
				(unsigned long long)blk_threshold,
				ag_max_usable);
	if (ret > ag_max_usable)
		ret = ag_max_usable;
	if (ret == 1)
		ret = 0;
	return ret;
}

/* Trim each AG on the data device. */
static int
fstrim_datadev(
	struct scrub_ctx	*ctx)
{
	struct xfs_fsop_geom	*geo = &ctx->mnt.fsgeom;
	uint64_t		fsbno;
	uint64_t		minlen_fsb;
	int			error;

	minlen_fsb = fstrim_compute_minlen(ctx, &ctx->datadev_hist);

	for (fsbno = 0; fsbno < geo->datablocks; fsbno += geo->agblocks) {
		uint64_t	fsbcount;

		/*
		 * Skip the first block of each AG to ensure that we get the
		 * partial-AG discard implementation, which cycles the AGF lock
		 * to prevent foreground threads from stalling.
		 */
		progress_add(geo->blocksize);
		fsbcount = min(geo->datablocks - fsbno + 1, geo->agblocks);
		error = fstrim_fsblocks(ctx, fsbno + 1, fsbcount, minlen_fsb,
				false);
		if (error)
			return error;
	}

	return 0;
}

/* Trim the realtime device. */
static int
fstrim_rtdev(
	struct scrub_ctx	*ctx)
{
	struct xfs_fsop_geom	*geo = &ctx->mnt.fsgeom;

	/*
	 * The fstrim ioctl pretends that the realtime volume is in the address
	 * space immediately after the data volume.  Ignore EINVAL if someone
	 * tries to run us on an older kernel.
	 */
	return fstrim_fsblocks(ctx, geo->datablocks, geo->rtblocks, 0, true);
}

/* Trim the filesystem, if desired. */
int
phase8_func(
	struct scrub_ctx	*ctx)
{
	int			error;

	if (!fstrim_ok(ctx))
		return 0;

	error = fstrim_datadev(ctx);
	if (error)
		return error;
	return fstrim_rtdev(ctx);
}

/* Estimate how much work we're going to do. */
int
phase8_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	if (fstrim_ok(ctx)) {
		*items = cvt_off_fsb_to_b(&ctx->mnt,
				ctx->mnt.fsgeom.datablocks);
		*items += cvt_off_fsb_to_b(&ctx->mnt,
				ctx->mnt.fsgeom.rtblocks);
	} else {
		*items = 0;
	}
	*nr_threads = 1;
	*rshift = 30; /* GiB */
	return 0;
}
