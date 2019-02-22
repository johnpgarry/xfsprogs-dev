// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <pthread.h>
#include "libfrog.h"
#include "workqueue.h"
#include "input.h"
#include "path.h"
#include "handle.h"
#include "bitops.h"
#include "avl64.h"
#include "list.h"
#include "xfs_scrub.h"
#include "common.h"
#include "disk.h"
#include "scrub.h"
#include "repair.h"

/* Phase 1: Find filesystem geometry (and clean up after) */

/* Shut down the filesystem. */
void
xfs_shutdown_fs(
	struct scrub_ctx		*ctx)
{
	int				flag;

	flag = XFS_FSOP_GOING_FLAGS_LOGFLUSH;
	str_info(ctx, ctx->mntpoint, _("Shutting down filesystem!"));
	if (ioctl(ctx->mnt_fd, XFS_IOC_GOINGDOWN, &flag))
		str_errno(ctx, ctx->mntpoint);
}

/* Clean up the XFS-specific state data. */
bool
xfs_cleanup_fs(
	struct scrub_ctx	*ctx)
{
	int			error;

	xfs_action_lists_free(&ctx->action_lists);
	if (ctx->fshandle)
		free_handle(ctx->fshandle, ctx->fshandle_len);
	if (ctx->rtdev)
		disk_close(ctx->rtdev);
	if (ctx->logdev)
		disk_close(ctx->logdev);
	if (ctx->datadev)
		disk_close(ctx->datadev);
	fshandle_destroy();
	if (ctx->mnt_fd >= 0) {
		error = close(ctx->mnt_fd);
		if (error)
			str_errno(ctx, _("closing mountpoint fd"));
	}
	fs_table_destroy();

	return true;
}

/*
 * Bind to the mountpoint, read the XFS geometry, bind to the block devices.
 * Anything we've already built will be cleaned up by xfs_cleanup_fs.
 */
bool
xfs_setup_fs(
	struct scrub_ctx		*ctx)
{
	int				error;

	/*
	 * Open the directory with O_NOATIME.  For mountpoints owned
	 * by root, this should be sufficient to ensure that we have
	 * CAP_SYS_ADMIN, which we probably need to do anything fancy
	 * with the (XFS driver) kernel.
	 */
	ctx->mnt_fd = open(ctx->mntpoint, O_RDONLY | O_NOATIME | O_DIRECTORY);
	if (ctx->mnt_fd < 0) {
		if (errno == EPERM)
			str_info(ctx, ctx->mntpoint,
_("Must be root to run scrub."));
		else
			str_errno(ctx, ctx->mntpoint);
		return false;
	}

	error = fstat(ctx->mnt_fd, &ctx->mnt_sb);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return false;
	}
	error = fstatvfs(ctx->mnt_fd, &ctx->mnt_sv);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return false;
	}
	error = fstatfs(ctx->mnt_fd, &ctx->mnt_sf);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return false;
	}

	if (!platform_test_xfs_fd(ctx->mnt_fd)) {
		str_info(ctx, ctx->mntpoint,
_("Does not appear to be an XFS filesystem!"));
		return false;
	}

	/*
	 * Flush everything out to disk before we start checking.
	 * This seems to reduce the incidence of stale file handle
	 * errors when we open things by handle.
	 */
	error = syncfs(ctx->mnt_fd);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return false;
	}

	/* Retrieve XFS geometry. */
	error = ioctl(ctx->mnt_fd, XFS_IOC_FSGEOMETRY, &ctx->geo);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return false;
	}

	if (!xfs_action_lists_alloc(ctx->geo.agcount, &ctx->action_lists)) {
		str_error(ctx, ctx->mntpoint, _("Not enough memory."));
		return false;
	}

	ctx->agblklog = log2_roundup(ctx->geo.agblocks);
	ctx->blocklog = highbit32(ctx->geo.blocksize);
	ctx->inodelog = highbit32(ctx->geo.inodesize);
	ctx->inopblog = ctx->blocklog - ctx->inodelog;

	error = path_to_fshandle(ctx->mntpoint, &ctx->fshandle,
			&ctx->fshandle_len);
	if (error) {
		str_errno(ctx, _("getting fshandle"));
		return false;
	}

	/* Do we have kernel-assisted metadata scrubbing? */
	if (!xfs_can_scrub_fs_metadata(ctx) || !xfs_can_scrub_inode(ctx) ||
	    !xfs_can_scrub_bmap(ctx) || !xfs_can_scrub_dir(ctx) ||
	    !xfs_can_scrub_attr(ctx) || !xfs_can_scrub_symlink(ctx) ||
	    !xfs_can_scrub_parent(ctx)) {
		str_info(ctx, ctx->mntpoint,
_("Kernel metadata scrubbing facility is not available."));
		return false;
	}

	/* Do we need kernel-assisted metadata repair? */
	if (ctx->mode != SCRUB_MODE_DRY_RUN && !xfs_can_repair(ctx)) {
		str_info(ctx, ctx->mntpoint,
_("Kernel metadata repair facility is not available.  Use -n to scrub."));
		return false;
	}

	/* Did we find the log and rt devices, if they're present? */
	if (ctx->geo.logstart == 0 && ctx->fsinfo.fs_log == NULL) {
		str_info(ctx, ctx->mntpoint,
_("Unable to find log device path."));
		return false;
	}
	if (ctx->geo.rtblocks && ctx->fsinfo.fs_rt == NULL) {
		str_info(ctx, ctx->mntpoint,
_("Unable to find realtime device path."));
		return false;
	}

	/* Open the raw devices. */
	ctx->datadev = disk_open(ctx->fsinfo.fs_name);
	if (error) {
		str_errno(ctx, ctx->fsinfo.fs_name);
		return false;
	}

	ctx->nr_io_threads = disk_heads(ctx->datadev);
	if (verbose) {
		fprintf(stdout, _("%s: using %d threads to scrub.\n"),
				ctx->mntpoint, scrub_nproc(ctx));
		fflush(stdout);
	}

	if (ctx->fsinfo.fs_log) {
		ctx->logdev = disk_open(ctx->fsinfo.fs_log);
		if (error) {
			str_errno(ctx, ctx->fsinfo.fs_name);
			return false;
		}
	}
	if (ctx->fsinfo.fs_rt) {
		ctx->rtdev = disk_open(ctx->fsinfo.fs_rt);
		if (error) {
			str_errno(ctx, ctx->fsinfo.fs_name);
			return false;
		}
	}

	/*
	 * Everything's set up, which means any failures recorded after
	 * this point are most probably corruption errors (as opposed to
	 * purely setup errors).
	 */
	log_info(ctx, _("Invoking online scrub."), ctx);
	ctx->scrub_setup_succeeded = true;
	return true;
}
