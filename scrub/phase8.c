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

/* Trim the filesystem, if desired. */
int
phase8_func(
	struct scrub_ctx	*ctx)
{
	if (action_list_empty(ctx->fs_repair_list) &&
	    action_list_empty(ctx->file_repair_list))
		goto maybe_trim;

	/*
	 * If errors remain on the filesystem, do not trim anything.  We don't
	 * have any threads running, so it's ok to skip the ctx lock here.
	 */
	if (ctx->corruptions_found || ctx->unfixable_errors != 0)
		return 0;

maybe_trim:
	fstrim(ctx);
	progress_add(1);
	return 0;
}

/* Estimate how much work we're going to do. */
int
phase8_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = 1;
	*nr_threads = 1;
	*rshift = 0;
	return 0;
}
