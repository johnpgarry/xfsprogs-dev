// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
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
#include "libfrog/util.h"
#include "libfrog/workqueue.h"
#include "input.h"
#include "libfrog/paths.h"
#include "handle.h"
#include "bitops.h"
#include "libfrog/avl64.h"
#include "list.h"
#include "xfs_scrub.h"
#include "common.h"
#include "disk.h"
#include "scrub.h"
#include "repair.h"
#include "libfrog/fsgeom.h"
#include "xfs_errortag.h"

/* Phase 1: Find filesystem geometry (and clean up after) */

/* Shut down the filesystem. */
void
xfs_shutdown_fs(
	struct scrub_ctx		*ctx)
{
	int				flag;

	flag = XFS_FSOP_GOING_FLAGS_LOGFLUSH;
	str_info(ctx, ctx->mntpoint, _("Shutting down filesystem!"));
	if (ioctl(ctx->mnt.fd, XFS_IOC_GOINGDOWN, &flag))
		str_errno(ctx, ctx->mntpoint);
}

/*
 * If we haven't found /any/ problems at all, tell the kernel that we're giving
 * the filesystem a clean bill of health.
 */
static int
report_to_kernel(
	struct scrub_ctx	*ctx)
{
	struct action_list	alist;
	int			ret;

	if (!ctx->scrub_setup_succeeded || ctx->corruptions_found ||
	    ctx->runtime_errors || ctx->unfixable_errors ||
	    ctx->warnings_found)
		return 0;

	action_list_init(&alist);
	ret = scrub_clean_health(ctx, &alist);
	if (ret)
		return ret;

	/*
	 * Complain if we cannot fail the clean bill of health, unless we're
	 * just testing repairs.
	 */
	if (action_list_length(&alist) > 0 &&
	    !debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
		str_info(ctx, _("Couldn't upload clean bill of health."), NULL);
		action_list_discard(&alist);
	}

	return 0;
}

/* Clean up the XFS-specific state data. */
int
scrub_cleanup(
	struct scrub_ctx	*ctx)
{
	int			error;

	error = report_to_kernel(ctx);
	if (error)
		return error;

	action_lists_free(&ctx->action_lists);
	if (ctx->fshandle)
		free_handle(ctx->fshandle, ctx->fshandle_len);
	if (ctx->rtdev)
		disk_close(ctx->rtdev);
	if (ctx->logdev)
		disk_close(ctx->logdev);
	if (ctx->datadev)
		disk_close(ctx->datadev);
	fshandle_destroy();
	error = -xfd_close(&ctx->mnt);
	if (error)
		str_liberror(ctx, error, _("closing mountpoint fd"));
	fs_table_destroy();

	return error;
}

/* Decide if we're using FORCE_REBUILD or injecting FORCE_REPAIR. */
static int
enable_force_repair(
	struct scrub_ctx		*ctx)
{
	struct xfs_error_injection	inject = {
		.fd			= ctx->mnt.fd,
		.errtag			= XFS_ERRTAG_FORCE_SCRUB_REPAIR,
	};
	int				error;

	use_force_rebuild = can_force_rebuild(ctx);
	if (use_force_rebuild)
		return 0;

	error = ioctl(ctx->mnt.fd, XFS_IOC_ERROR_INJECTION, &inject);
	if (error)
		str_errno(ctx, _("force_repair"));
	return error;
}

/*
 * Bind to the mountpoint, read the XFS geometry, bind to the block devices.
 * Anything we've already built will be cleaned up by scrub_cleanup.
 */
int
phase1_func(
	struct scrub_ctx		*ctx)
{
	int				error;

	/*
	 * Open the directory with O_NOATIME.  For mountpoints owned
	 * by root, this should be sufficient to ensure that we have
	 * CAP_SYS_ADMIN, which we probably need to do anything fancy
	 * with the (XFS driver) kernel.
	 */
	error = -xfd_open(&ctx->mnt, ctx->mntpoint,
			O_RDONLY | O_NOATIME | O_DIRECTORY);
	if (error) {
		if (error == EPERM)
			str_error(ctx, ctx->mntpoint,
_("Must be root to run scrub."));
		else if (error == ENOTTY)
			str_error(ctx, ctx->mntpoint,
_("Not an XFS filesystem."));
		else
			str_liberror(ctx, error, ctx->mntpoint);
		return error;
	}

	error = fstat(ctx->mnt.fd, &ctx->mnt_sb);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}
	error = fstatvfs(ctx->mnt.fd, &ctx->mnt_sv);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}
	error = fstatfs(ctx->mnt.fd, &ctx->mnt_sf);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}

	/*
	 * Flush everything out to disk before we start checking.
	 * This seems to reduce the incidence of stale file handle
	 * errors when we open things by handle.
	 */
	error = syncfs(ctx->mnt.fd);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}

	error = action_lists_alloc(ctx->mnt.fsgeom.agcount,
			&ctx->action_lists);
	if (error) {
		str_liberror(ctx, error, _("allocating action lists"));
		return error;
	}

	error = path_to_fshandle(ctx->mntpoint, &ctx->fshandle,
			&ctx->fshandle_len);
	if (error) {
		str_errno(ctx, _("getting fshandle"));
		return error;
	}

	/* Do we have kernel-assisted metadata scrubbing? */
	if (!can_scrub_fs_metadata(ctx) || !can_scrub_inode(ctx) ||
	    !can_scrub_bmap(ctx) || !can_scrub_dir(ctx) ||
	    !can_scrub_attr(ctx) || !can_scrub_symlink(ctx) ||
	    !can_scrub_parent(ctx)) {
		str_error(ctx, ctx->mntpoint,
_("Kernel metadata scrubbing facility is not available."));
		return ECANCELED;
	}

	/* Do we need kernel-assisted metadata repair? */
	if (ctx->mode != SCRUB_MODE_DRY_RUN && !xfs_can_repair(ctx)) {
		str_error(ctx, ctx->mntpoint,
_("Kernel metadata repair facility is not available.  Use -n to scrub."));
		return ECANCELED;
	}

	if (debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
		error = enable_force_repair(ctx);
		if (error)
			return error;
	}

	/* Did we find the log and rt devices, if they're present? */
	if (ctx->mnt.fsgeom.logstart == 0 && ctx->fsinfo.fs_log == NULL) {
		str_error(ctx, ctx->mntpoint,
_("Unable to find log device path."));
		return ECANCELED;
	}
	if (ctx->mnt.fsgeom.rtblocks && ctx->fsinfo.fs_rt == NULL) {
		str_error(ctx, ctx->mntpoint,
_("Unable to find realtime device path."));
		return ECANCELED;
	}

	/* Open the raw devices. */
	ctx->datadev = disk_open(ctx->fsinfo.fs_name);
	if (!ctx->datadev) {
		str_error(ctx, ctx->mntpoint, _("Unable to open data device."));
		return ECANCELED;
	}

	ctx->nr_io_threads = disk_heads(ctx->datadev);
	if (verbose) {
		fprintf(stdout, _("%s: using %d threads to scrub.\n"),
				ctx->mntpoint, scrub_nproc(ctx));
		fflush(stdout);
	}

	if (ctx->fsinfo.fs_log) {
		ctx->logdev = disk_open(ctx->fsinfo.fs_log);
		if (!ctx->logdev) {
			str_error(ctx, ctx->mntpoint,
				_("Unable to open external log device."));
			return ECANCELED;
		}
	}
	if (ctx->fsinfo.fs_rt) {
		ctx->rtdev = disk_open(ctx->fsinfo.fs_rt);
		if (!ctx->rtdev) {
			str_error(ctx, ctx->mntpoint,
				_("Unable to open realtime device."));
			return ECANCELED;
		}
	}

	/*
	 * Everything's set up, which means any failures recorded after
	 * this point are most probably corruption errors (as opposed to
	 * purely setup errors).
	 */
	log_info(ctx, _("Invoking online scrub."), ctx);
	ctx->scrub_setup_succeeded = true;
	return 0;
}
