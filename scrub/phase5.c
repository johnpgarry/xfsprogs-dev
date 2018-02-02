/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "xfs.h"
#include "path.h"
#include "workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "inodes.h"
#include "scrub.h"

/* Phase 5: Check directory connectivity. */

/*
 * Verify the connectivity of the directory tree.
 * We know that the kernel's open-by-handle function will try to reconnect
 * parents of an opened directory, so we'll accept that as sufficient.
 */
static int
xfs_scrub_connections(
	struct scrub_ctx	*ctx,
	struct xfs_handle	*handle,
	struct xfs_bstat	*bstat,
	void			*arg)
{
	bool			*pmoveon = arg;
	char			descr[DESCR_BUFSZ];
	bool			moveon = true;
	xfs_agnumber_t		agno;
	xfs_agino_t		agino;
	int			fd = -1;

	agno = bstat->bs_ino / (1ULL << (ctx->inopblog + ctx->agblklog));
	agino = bstat->bs_ino % (1ULL << (ctx->inopblog + ctx->agblklog));
	snprintf(descr, DESCR_BUFSZ, _("inode %"PRIu64" (%u/%u)"),
			(uint64_t)bstat->bs_ino, agno, agino);
	background_sleep();

	/* Open the dir, let the kernel try to reconnect it to the root. */
	if (S_ISDIR(bstat->bs_mode)) {
		fd = xfs_open_handle(handle);
		if (fd < 0) {
			if (errno == ESTALE)
				return ESTALE;
			str_errno(ctx, descr);
			goto out;
		}
	}

out:
	if (fd >= 0)
		close(fd);
	if (!moveon)
		*pmoveon = false;
	return *pmoveon ? 0 : XFS_ITERATE_INODES_ABORT;
}

/* Check directory connectivity. */
bool
xfs_scan_connections(
	struct scrub_ctx	*ctx)
{
	bool			moveon = true;
	bool			ret;

	if (ctx->errors_found) {
		str_info(ctx, ctx->mntpoint,
_("Filesystem has errors, skipping connectivity checks."));
		return true;
	}

	ret = xfs_scan_all_inodes(ctx, xfs_scrub_connections, &moveon);
	if (!ret)
		moveon = false;
	if (!moveon)
		return false;
	xfs_scrub_report_preen_triggers(ctx);
	return true;
}
