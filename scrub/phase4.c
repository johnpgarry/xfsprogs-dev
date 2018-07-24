// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "path.h"
#include "workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "progress.h"
#include "scrub.h"
#include "vfs.h"

/* Phase 4: Repair filesystem. */

/* Process all the action items. */
static bool
xfs_process_action_items(
	struct scrub_ctx		*ctx)
{
	bool				moveon = true;

	pthread_mutex_lock(&ctx->lock);
	if (moveon && ctx->errors_found == 0 && want_fstrim) {
		fstrim(ctx);
		progress_add(1);
	}
	pthread_mutex_unlock(&ctx->lock);

	return moveon;
}

/* Fix everything that needs fixing. */
bool
xfs_repair_fs(
	struct scrub_ctx		*ctx)
{
	return xfs_process_action_items(ctx);
}

/* Estimate how much work we're going to do. */
bool
xfs_estimate_repair_work(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = 1;
	*nr_threads = 1;
	*rshift = 0;
	return true;
}
