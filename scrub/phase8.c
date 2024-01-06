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

/* Trim a certain range of the filesystem. */
static int
fstrim_fsblocks(
	struct scrub_ctx	*ctx,
	uint64_t		start_fsb,
	uint64_t		fsbcount)
{
	uint64_t		start = cvt_off_fsb_to_b(&ctx->mnt, start_fsb);
	uint64_t		len = cvt_off_fsb_to_b(&ctx->mnt, fsbcount);
	int			error;

	error = fstrim(ctx, start, len);
	if (error == EOPNOTSUPP)
		return 0;
	if (error) {
		char		descr[DESCR_BUFSZ];

		snprintf(descr, sizeof(descr) - 1,
				_("fstrim start 0x%llx len 0x%llx"),
				(unsigned long long)start,
				(unsigned long long)len);
		str_liberror(ctx, error, descr);
		return error;
	}

	return 0;
}

/* Trim each AG on the data device. */
static int
fstrim_datadev(
	struct scrub_ctx	*ctx)
{
	struct xfs_fsop_geom	*geo = &ctx->mnt.fsgeom;
	uint64_t		fsbno;
	int			error;

	for (fsbno = 0; fsbno < geo->datablocks; fsbno += geo->agblocks) {
		uint64_t	fsbcount;

		/*
		 * Skip the first block of each AG to ensure that we get the
		 * partial-AG discard implementation, which cycles the AGF lock
		 * to prevent foreground threads from stalling.
		 */
		fsbcount = min(geo->datablocks - fsbno + 1, geo->agblocks);
		error = fstrim_fsblocks(ctx, fsbno + 1, fsbcount);
		if (error)
			return error;
	}

	progress_add(1);
	return 0;
}

/* Trim the filesystem, if desired. */
int
phase8_func(
	struct scrub_ctx	*ctx)
{
	if (!fstrim_ok(ctx))
		return 0;

	return fstrim_datadev(ctx);
}

/* Estimate how much work we're going to do. */
int
phase8_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = 0;

	if (fstrim_ok(ctx))
		*items = 1;

	*nr_threads = 1;
	*rshift = 0;
	return 0;
}
