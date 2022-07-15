// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "xfs_scrub.h"
#include "common.h"
#include "progress.h"
#include "scrub.h"
#include "repair.h"
#include "descr.h"
#include "scrub_private.h"

/* Online scrub and repair wrappers. */

/* Format a scrub description. */
int
format_scrub_descr(
	struct scrub_ctx		*ctx,
	char				*buf,
	size_t				buflen,
	void				*where)
{
	struct xfs_scrub_metadata	*meta = where;
	const struct xfrog_scrub_descr	*sc = &xfrog_scrubbers[meta->sm_type];

	switch (sc->group) {
	case XFROG_SCRUB_GROUP_AGHEADER:
	case XFROG_SCRUB_GROUP_PERAG:
		return snprintf(buf, buflen, _("AG %u %s"), meta->sm_agno,
				_(sc->descr));
	case XFROG_SCRUB_GROUP_INODE:
		return scrub_render_ino_descr(ctx, buf, buflen,
				meta->sm_ino, meta->sm_gen, "%s",
				_(sc->descr));
	case XFROG_SCRUB_GROUP_FS:
	case XFROG_SCRUB_GROUP_SUMMARY:
	case XFROG_SCRUB_GROUP_ISCAN:
	case XFROG_SCRUB_GROUP_NONE:
		return snprintf(buf, buflen, _("%s"), _(sc->descr));
	}
	return -1;
}

/* Warn about strange circumstances after scrub. */
void
scrub_warn_incomplete_scrub(
	struct scrub_ctx		*ctx,
	struct descr			*dsc,
	struct xfs_scrub_metadata	*meta)
{
	if (is_incomplete(meta))
		str_info(ctx, descr_render(dsc), _("Check incomplete."));

	if (is_suspicious(meta)) {
		if (debug)
			str_info(ctx, descr_render(dsc),
					_("Possibly suspect metadata."));
		else
			str_warn(ctx, descr_render(dsc),
					_("Possibly suspect metadata."));
	}

	if (xref_failed(meta))
		str_info(ctx, descr_render(dsc),
				_("Cross-referencing failed."));
}

/* Do a read-only check of some metadata. */
static int
xfs_check_metadata(
	struct scrub_ctx		*ctx,
	struct xfs_fd			*xfdp,
	unsigned int			scrub_type,
	struct scrub_item		*sri)
{
	DEFINE_DESCR(dsc, ctx, format_scrub_descr);
	struct xfs_scrub_metadata	meta = { };
	enum xfrog_scrub_group		group;
	int				error;

	background_sleep();

	group = xfrog_scrubbers[scrub_type].group;
	meta.sm_type = scrub_type;
	switch (group) {
	case XFROG_SCRUB_GROUP_AGHEADER:
	case XFROG_SCRUB_GROUP_PERAG:
		meta.sm_agno = sri->sri_agno;
		break;
	case XFROG_SCRUB_GROUP_FS:
	case XFROG_SCRUB_GROUP_SUMMARY:
	case XFROG_SCRUB_GROUP_ISCAN:
	case XFROG_SCRUB_GROUP_NONE:
		break;
	case XFROG_SCRUB_GROUP_INODE:
		meta.sm_ino = sri->sri_ino;
		meta.sm_gen = sri->sri_gen;
		break;
	}

	assert(!debug_tweak_on("XFS_SCRUB_NO_KERNEL"));
	assert(scrub_type < XFS_SCRUB_TYPE_NR);
	descr_set(&dsc, &meta);

	dbg_printf("check %s flags %xh\n", descr_render(&dsc), meta.sm_flags);

	error = -xfrog_scrub_metadata(xfdp, &meta);
	switch (error) {
	case 0:
		/* No operational errors encountered. */
		if (!sri->sri_revalidate &&
		    debug_tweak_on("XFS_SCRUB_FORCE_REPAIR"))
			meta.sm_flags |= XFS_SCRUB_OFLAG_CORRUPT;
		break;
	case ENOENT:
		/* Metadata not present, just skip it. */
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	case ESHUTDOWN:
		/* FS already crashed, give up. */
		str_error(ctx, descr_render(&dsc),
_("Filesystem is shut down, aborting."));
		return ECANCELED;
	case EIO:
	case ENOMEM:
		/* Abort on I/O errors or insufficient memory. */
		str_liberror(ctx, error, descr_render(&dsc));
		return ECANCELED;
	case EDEADLOCK:
	case EBUSY:
	case EFSBADCRC:
	case EFSCORRUPTED:
		/*
		 * The first two should never escape the kernel,
		 * and the other two should be reported via sm_flags.
		 * Log it and move on.
		 */
		str_liberror(ctx, error, _("Kernel bug"));
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	default:
		/* Operational error.  Log it and move on. */
		str_liberror(ctx, error, descr_render(&dsc));
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	}

	/*
	 * If the kernel says the test was incomplete or that there was
	 * a cross-referencing discrepancy but no obvious corruption,
	 * we'll try the scan again, just in case the fs was busy.
	 * Only retry so many times.
	 */
	if (want_retry(&meta) && scrub_item_schedule_retry(sri, scrub_type))
		return 0;

	/* Complain about incomplete or suspicious metadata. */
	scrub_warn_incomplete_scrub(ctx, &dsc, &meta);

	/*
	 * If we need repairs or there were discrepancies, schedule a
	 * repair if desired, otherwise complain.
	 */
	if (is_corrupt(&meta) || xref_disagrees(&meta)) {
		if (ctx->mode < SCRUB_MODE_REPAIR) {
			/* Dry-run mode, so log an error and forget it. */
			str_corrupt(ctx, descr_render(&dsc),
_("Repairs are required."));
			scrub_item_clean_state(sri, scrub_type);
			return 0;
		}

		/* Schedule repairs. */
		scrub_item_save_state(sri, scrub_type, meta.sm_flags);
		return 0;
	}

	/*
	 * If we could optimize, schedule a repair if desired,
	 * otherwise complain.
	 */
	if (is_unoptimized(&meta)) {
		if (ctx->mode != SCRUB_MODE_REPAIR) {
			/* Dry-run mode, so log an error and forget it. */
			if (group != XFROG_SCRUB_GROUP_INODE) {
				/* AG or FS metadata, always warn. */
				str_info(ctx, descr_render(&dsc),
_("Optimization is possible."));
			} else if (!ctx->preen_triggers[scrub_type]) {
				/* File metadata, only warn once per type. */
				pthread_mutex_lock(&ctx->lock);
				if (!ctx->preen_triggers[scrub_type])
					ctx->preen_triggers[scrub_type] = true;
				pthread_mutex_unlock(&ctx->lock);
			}
			scrub_item_clean_state(sri, scrub_type);
			return 0;
		}

		/* Schedule optimizations. */
		scrub_item_save_state(sri, scrub_type, meta.sm_flags);
		return 0;
	}

	/*
	 * This metadata object itself looks ok, but we noticed inconsistencies
	 * when comparing it with the other filesystem metadata.  If we're in
	 * repair mode we need to queue it for a "repair" so that phase 4 will
	 * re-examine the object as repairs progress to see if the kernel will
	 * deem it completely consistent at some point.
	 */
	if (xref_failed(&meta) && ctx->mode == SCRUB_MODE_REPAIR) {
		scrub_item_save_state(sri, scrub_type, meta.sm_flags);
		return 0;
	}

	/* Everything is ok. */
	scrub_item_clean_state(sri, scrub_type);
	return 0;
}

/* Bulk-notify user about things that could be optimized. */
void
scrub_report_preen_triggers(
	struct scrub_ctx		*ctx)
{
	int				i;

	for (i = 0; i < XFS_SCRUB_TYPE_NR; i++) {
		pthread_mutex_lock(&ctx->lock);
		if (ctx->preen_triggers[i]) {
			ctx->preen_triggers[i] = false;
			pthread_mutex_unlock(&ctx->lock);
			str_info(ctx, ctx->mntpoint,
_("Optimizations of %s are possible."), _(xfrog_scrubbers[i].descr));
		} else {
			pthread_mutex_unlock(&ctx->lock);
		}
	}
}

/* Schedule scrub for all metadata of a given group. */
void
scrub_item_schedule_group(
	struct scrub_item		*sri,
	enum xfrog_scrub_group		group)
{
	unsigned int			scrub_type;

	foreach_scrub_type(scrub_type) {
		if (xfrog_scrubbers[scrub_type].group != group)
			continue;
		scrub_item_schedule(sri, scrub_type);
	}
}

/* Decide if we call the kernel again to finish scrub/repair activity. */
bool
scrub_item_call_kernel_again(
	struct scrub_item	*sri,
	unsigned int		scrub_type,
	uint8_t			work_mask,
	const struct scrub_item	*old)
{
	uint8_t			statex;

	/* If there's nothing to do, we're done. */
	if (!(sri->sri_state[scrub_type] & work_mask))
		return false;

	/*
	 * We are willing to go again if the last call had any effect on the
	 * state of the scrub item that the caller cares about, if the freeze
	 * flag got set, or if the kernel asked us to try again...
	 */
	statex = sri->sri_state[scrub_type] ^ old->sri_state[scrub_type];
	if (statex & work_mask)
		return true;
	if (sri->sri_tries[scrub_type] != old->sri_tries[scrub_type])
		return true;

	return false;
}

/* Run all the incomplete scans on this scrub principal. */
int
scrub_item_check_file(
	struct scrub_ctx		*ctx,
	struct scrub_item		*sri,
	int				override_fd)
{
	struct xfs_fd			xfd;
	struct scrub_item		old_sri;
	struct xfs_fd			*xfdp = &ctx->mnt;
	unsigned int			scrub_type;
	int				error;

	/*
	 * If the caller passed us a file descriptor for a scrub, use it
	 * instead of scrub-by-handle because this enables the kernel to skip
	 * costly inode btree lookups.
	 */
	if (override_fd >= 0) {
		memcpy(&xfd, xfdp, sizeof(xfd));
		xfd.fd = override_fd;
		xfdp = &xfd;
	}

	foreach_scrub_type(scrub_type) {
		if (!(sri->sri_state[scrub_type] & SCRUB_ITEM_NEEDSCHECK))
			continue;

		sri->sri_tries[scrub_type] = SCRUB_ITEM_MAX_RETRIES;
		do {
			memcpy(&old_sri, sri, sizeof(old_sri));
			error = xfs_check_metadata(ctx, xfdp, scrub_type, sri);
			if (error)
				return error;
		} while (scrub_item_call_kernel_again(sri, scrub_type,
					SCRUB_ITEM_NEEDSCHECK, &old_sri));

		/*
		 * Progress is counted by the inode for inode metadata; for
		 * everything else, it's counted for each scrub call.
		 */
		if (sri->sri_ino == -1ULL)
			progress_add(1);

		if (error)
			break;
	}

	return error;
}

/* How many items do we have to check? */
unsigned int
scrub_estimate_ag_work(
	struct scrub_ctx		*ctx)
{
	const struct xfrog_scrub_descr	*sc;
	int				type;
	unsigned int			estimate = 0;

	sc = xfrog_scrubbers;
	for (type = 0; type < XFS_SCRUB_TYPE_NR; type++, sc++) {
		switch (sc->group) {
		case XFROG_SCRUB_GROUP_AGHEADER:
		case XFROG_SCRUB_GROUP_PERAG:
			estimate += ctx->mnt.fsgeom.agcount;
			break;
		case XFROG_SCRUB_GROUP_FS:
			estimate++;
			break;
		default:
			break;
		}
	}
	return estimate;
}

/*
 * How many kernel calls will we make to scrub everything requiring a full
 * inode scan?
 */
unsigned int
scrub_estimate_iscan_work(
	struct scrub_ctx		*ctx)
{
	const struct xfrog_scrub_descr	*sc;
	int				type;
	unsigned int			estimate;

	estimate = ctx->mnt_sv.f_files - ctx->mnt_sv.f_ffree;

	sc = xfrog_scrubbers;
	for (type = 0; type < XFS_SCRUB_TYPE_NR; type++, sc++) {
		if (sc->group == XFROG_SCRUB_GROUP_ISCAN)
			estimate++;
	}

	return estimate;
}

/* Dump a scrub item for debugging purposes. */
void
scrub_item_dump(
	struct scrub_item	*sri,
	unsigned int		group_mask,
	const char		*tag)
{
	unsigned int		i;

	if (group_mask == 0)
		group_mask = -1U;

	printf("DUMP SCRUB ITEM FOR %s\n", tag);
	if (sri->sri_ino != -1ULL)
		printf("ino 0x%llx gen %u\n", (unsigned long long)sri->sri_ino,
				sri->sri_gen);
	if (sri->sri_agno != -1U)
		printf("agno %u\n", sri->sri_agno);

	foreach_scrub_type(i) {
		unsigned int	g = 1U << xfrog_scrubbers[i].group;

		if (g & group_mask)
			printf("[%u]: type '%s' state 0x%x tries %u\n", i,
					xfrog_scrubbers[i].name,
					sri->sri_state[i], sri->sri_tries[i]);
	}
	fflush(stdout);
}

/*
 * Test the availability of a kernel scrub command.  If errors occur (or the
 * scrub ioctl is rejected) the errors will be logged and this function will
 * return false.
 */
static bool
__scrub_test(
	struct scrub_ctx		*ctx,
	unsigned int			type,
	unsigned int			flags)
{
	struct xfs_scrub_metadata	meta = {0};
	int				error;

	if (debug_tweak_on("XFS_SCRUB_NO_KERNEL"))
		return false;

	meta.sm_type = type;
	meta.sm_flags = flags;
	error = -xfrog_scrub_metadata(&ctx->mnt, &meta);
	switch (error) {
	case 0:
		return true;
	case EROFS:
		str_info(ctx, ctx->mntpoint,
_("Filesystem is mounted read-only; cannot proceed."));
		return false;
	case ENOTRECOVERABLE:
		str_info(ctx, ctx->mntpoint,
_("Filesystem is mounted norecovery; cannot proceed."));
		return false;
	case EINVAL:
	case EOPNOTSUPP:
	case ENOTTY:
		if (debug || verbose)
			str_info(ctx, ctx->mntpoint,
_("Kernel %s %s facility not detected."),
					_(xfrog_scrubbers[type].descr),
					(flags & XFS_SCRUB_IFLAG_REPAIR) ?
						_("repair") : _("scrub"));
		return false;
	case ENOENT:
		/* Scrubber says not present on this fs; that's fine. */
		return true;
	default:
		str_info(ctx, ctx->mntpoint, "%s", strerror(errno));
		return true;
	}
}

bool
can_scrub_fs_metadata(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_PROBE, 0);
}

bool
can_scrub_inode(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_INODE, 0);
}

bool
can_scrub_bmap(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_BMBTD, 0);
}

bool
can_scrub_dir(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_DIR, 0);
}

bool
can_scrub_attr(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_XATTR, 0);
}

bool
can_scrub_symlink(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_SYMLINK, 0);
}

bool
can_scrub_parent(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_PARENT, 0);
}

bool
can_repair(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_PROBE, XFS_SCRUB_IFLAG_REPAIR);
}

bool
can_force_rebuild(
	struct scrub_ctx	*ctx)
{
	return __scrub_test(ctx, XFS_SCRUB_TYPE_PROBE,
			XFS_SCRUB_IFLAG_REPAIR | XFS_SCRUB_IFLAG_FORCE_REBUILD);
}
