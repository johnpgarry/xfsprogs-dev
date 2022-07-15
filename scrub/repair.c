// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "xfs_scrub.h"
#include "common.h"
#include "scrub.h"
#include "progress.h"
#include "repair.h"
#include "descr.h"
#include "scrub_private.h"

/* General repair routines. */

/* Repair some metadata. */
static enum check_outcome
xfs_repair_metadata(
	struct scrub_ctx		*ctx,
	struct xfs_fd			*xfdp,
	unsigned int			scrub_type,
	struct scrub_item		*sri,
	unsigned int			repair_flags)
{
	struct xfs_scrub_metadata	meta = { 0 };
	struct xfs_scrub_metadata	oldm;
	DEFINE_DESCR(dsc, ctx, format_scrub_descr);
	int				error;

	assert(scrub_type < XFS_SCRUB_TYPE_NR);
	assert(!debug_tweak_on("XFS_SCRUB_NO_KERNEL"));
	meta.sm_type = scrub_type;
	meta.sm_flags = XFS_SCRUB_IFLAG_REPAIR;
	if (use_force_rebuild)
		meta.sm_flags |= XFS_SCRUB_IFLAG_FORCE_REBUILD;
	switch (xfrog_scrubbers[scrub_type].group) {
	case XFROG_SCRUB_GROUP_AGHEADER:
	case XFROG_SCRUB_GROUP_PERAG:
		meta.sm_agno = sri->sri_agno;
		break;
	case XFROG_SCRUB_GROUP_INODE:
		meta.sm_ino = sri->sri_ino;
		meta.sm_gen = sri->sri_gen;
		break;
	default:
		break;
	}

	if (!is_corrupt(&meta) && (repair_flags & XRM_REPAIR_ONLY))
		return CHECK_RETRY;

	memcpy(&oldm, &meta, sizeof(oldm));
	oldm.sm_flags = sri->sri_state[scrub_type] & SCRUB_ITEM_REPAIR_ANY;
	descr_set(&dsc, &oldm);

	if (needs_repair(&oldm))
		str_info(ctx, descr_render(&dsc), _("Attempting repair."));
	else if (debug || verbose)
		str_info(ctx, descr_render(&dsc),
				_("Attempting optimization."));

	error = -xfrog_scrub_metadata(xfdp, &meta);
	switch (error) {
	case 0:
		/* No operational errors encountered. */
		break;
	case EDEADLOCK:
	case EBUSY:
		/* Filesystem is busy, try again later. */
		if (debug || verbose)
			str_info(ctx, descr_render(&dsc),
_("Filesystem is busy, deferring repair."));
		return CHECK_RETRY;
	case ESHUTDOWN:
		/* Filesystem is already shut down, abort. */
		str_error(ctx, descr_render(&dsc),
_("Filesystem is shut down, aborting."));
		return CHECK_ABORT;
	case ENOTTY:
	case EOPNOTSUPP:
		/*
		 * If the kernel cannot perform the optimization that we
		 * requested; or we forced a repair but the kernel doesn't know
		 * how to perform the repair, don't requeue the request.  Mark
		 * it done and move on.
		 */
		if (is_unoptimized(&oldm) ||
		    debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
			scrub_item_clean_state(sri, scrub_type);
			return CHECK_DONE;
		}
		/*
		 * If we're in no-complain mode, requeue the check for
		 * later.  It's possible that an error in another
		 * component caused us to flag an error in this
		 * component.  Even if the kernel didn't think it
		 * could fix this, it's at least worth trying the scan
		 * again to see if another repair fixed it.
		 */
		if (!(repair_flags & XRM_FINAL_WARNING))
			return CHECK_RETRY;
		fallthrough;
	case EINVAL:
		/* Kernel doesn't know how to repair this? */
		str_corrupt(ctx, descr_render(&dsc),
_("Don't know how to fix; offline repair required."));
		scrub_item_clean_state(sri, scrub_type);
		return CHECK_DONE;
	case EROFS:
		/* Read-only filesystem, can't fix. */
		if (verbose || debug || needs_repair(&oldm))
			str_error(ctx, descr_render(&dsc),
_("Read-only filesystem; cannot make changes."));
		return CHECK_ABORT;
	case ENOENT:
		/* Metadata not present, just skip it. */
		scrub_item_clean_state(sri, scrub_type);
		return CHECK_DONE;
	case ENOMEM:
	case ENOSPC:
		/* Don't care if preen fails due to low resources. */
		if (is_unoptimized(&oldm) && !needs_repair(&oldm)) {
			scrub_item_clean_state(sri, scrub_type);
			return CHECK_DONE;
		}
		fallthrough;
	default:
		/*
		 * Operational error.  If the caller doesn't want us to
		 * complain about repair failures, tell the caller to requeue
		 * the repair for later and don't say a thing.  Otherwise,
		 * print an error, mark the item clean because we're done with
		 * trying to repair it, and bail out.
		 */
		if (!(repair_flags & XRM_FINAL_WARNING))
			return CHECK_RETRY;
		str_liberror(ctx, error, descr_render(&dsc));
		scrub_item_clean_state(sri, scrub_type);
		return CHECK_DONE;
	}

	if (repair_flags & XRM_FINAL_WARNING)
		scrub_warn_incomplete_scrub(ctx, &dsc, &meta);
	if (needs_repair(&meta)) {
		/*
		 * Still broken; if we've been told not to complain then we
		 * just requeue this and try again later.  Otherwise we
		 * log the error loudly and don't try again.
		 */
		if (!(repair_flags & XRM_FINAL_WARNING))
			return CHECK_RETRY;
		str_corrupt(ctx, descr_render(&dsc),
_("Repair unsuccessful; offline repair required."));
	} else if (xref_failed(&meta)) {
		/*
		 * This metadata object itself looks ok, but we still noticed
		 * inconsistencies when comparing it with the other filesystem
		 * metadata.  If we're in "final warning" mode, advise the
		 * caller to run xfs_repair; otherwise, we'll keep trying to
		 * reverify the cross-referencing as repairs progress.
		 */
		if (repair_flags & XRM_FINAL_WARNING) {
			str_info(ctx, descr_render(&dsc),
 _("Seems correct but cross-referencing failed; offline repair recommended."));
		} else {
			if (verbose)
				str_info(ctx, descr_render(&dsc),
 _("Seems correct but cross-referencing failed; will keep checking."));
			return CHECK_RETRY;
		}
	} else if (meta.sm_flags & XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED) {
		if (verbose)
			str_info(ctx, descr_render(&dsc),
					_("No modification needed."));
	} else {
		/* Clean operation, no corruption detected. */
		if (is_corrupt(&oldm))
			record_repair(ctx, descr_render(&dsc),
 _("Repairs successful."));
		else if (xref_disagrees(&oldm))
			record_repair(ctx, descr_render(&dsc),
 _("Repairs successful after discrepancy in cross-referencing."));
		else if (xref_failed(&oldm))
			record_repair(ctx, descr_render(&dsc),
 _("Repairs successful after cross-referencing failure."));
		else
			record_preen(ctx, descr_render(&dsc),
 _("Optimization successful."));
	}

	scrub_item_clean_state(sri, scrub_type);
	return CHECK_DONE;
}

/*
 * Prioritize action items in order of how long we can wait.
 *
 * To minimize the amount of repair work, we want to prioritize metadata
 * objects by perceived corruptness.  If CORRUPT is set, the fields are
 * just plain bad; try fixing that first.  Otherwise if XCORRUPT is set,
 * the fields could be bad, but the xref data could also be bad; we'll
 * try fixing that next.  Finally, if XFAIL is set, some other metadata
 * structure failed validation during xref, so we'll recheck this
 * metadata last since it was probably fine.
 *
 * For metadata that lie in the critical path of checking other metadata
 * (superblock, AG{F,I,FL}, inobt) we scrub and fix those things before
 * we even get to handling their dependencies, so things should progress
 * in order.
 */

struct action_item {
	struct list_head	list;
	struct scrub_item	sri;
};

/*
 * Figure out which AG metadata must be fixed before we can move on
 * to the inode scan.
 */
void
repair_item_mustfix(
	struct scrub_item	*sri,
	struct scrub_item	*fix_now)
{
	unsigned int		scrub_type;

	assert(sri->sri_agno != -1U);
	scrub_item_init_ag(fix_now, sri->sri_agno);

	foreach_scrub_type(scrub_type) {
		if (!(sri->sri_state[scrub_type] & SCRUB_ITEM_CORRUPT))
			continue;

		switch (scrub_type) {
		case XFS_SCRUB_TYPE_AGI:
		case XFS_SCRUB_TYPE_FINOBT:
		case XFS_SCRUB_TYPE_INOBT:
			fix_now->sri_state[scrub_type] |= SCRUB_ITEM_CORRUPT;
			break;
		}
	}
}

/* Determine if primary or secondary metadata are inconsistent. */
unsigned int
repair_item_difficulty(
	const struct scrub_item	*sri)
{
	unsigned int		scrub_type;
	unsigned int		ret = 0;

	foreach_scrub_type(scrub_type) {
		if (!(sri->sri_state[scrub_type] & (XFS_SCRUB_OFLAG_CORRUPT |
						    XFS_SCRUB_OFLAG_XCORRUPT |
						    XFS_SCRUB_OFLAG_XFAIL)))
			continue;

		switch (scrub_type) {
		case XFS_SCRUB_TYPE_RMAPBT:
			ret |= REPAIR_DIFFICULTY_SECONDARY;
			break;
		case XFS_SCRUB_TYPE_SB:
		case XFS_SCRUB_TYPE_AGF:
		case XFS_SCRUB_TYPE_AGFL:
		case XFS_SCRUB_TYPE_AGI:
		case XFS_SCRUB_TYPE_FINOBT:
		case XFS_SCRUB_TYPE_INOBT:
		case XFS_SCRUB_TYPE_BNOBT:
		case XFS_SCRUB_TYPE_CNTBT:
		case XFS_SCRUB_TYPE_REFCNTBT:
		case XFS_SCRUB_TYPE_RTBITMAP:
		case XFS_SCRUB_TYPE_RTSUM:
			ret |= REPAIR_DIFFICULTY_PRIMARY;
			break;
		}
	}

	return ret;
}

/*
 * Allocate a certain number of repair lists for the scrub context.  Returns
 * zero or a positive error number.
 */
int
action_lists_alloc(
	size_t				nr,
	struct action_list		**listsp)
{
	struct action_list		*lists;
	xfs_agnumber_t			agno;

	lists = calloc(nr, sizeof(struct action_list));
	if (!lists)
		return errno;

	for (agno = 0; agno < nr; agno++)
		action_list_init(&lists[agno]);
	*listsp = lists;

	return 0;
}

/* Discard repair list contents. */
void
action_list_discard(
	struct action_list		*alist)
{
	struct action_item		*aitem;
	struct action_item		*n;

	list_for_each_entry_safe(aitem, n, &alist->list, list) {
		alist->nr--;
		list_del(&aitem->list);
		free(aitem);
	}
}

/* Free the repair lists. */
void
action_lists_free(
	struct action_list		**listsp)
{
	free(*listsp);
	*listsp = NULL;
}

/* Initialize repair list */
void
action_list_init(
	struct action_list		*alist)
{
	INIT_LIST_HEAD(&alist->list);
	alist->nr = 0;
	alist->sorted = false;
}

/* Number of pending repairs in this list. */
unsigned long long
action_list_length(
	struct action_list		*alist)
{
	struct action_item		*aitem;
	unsigned long long		ret = 0;

	list_for_each_entry(aitem, &alist->list, list)
		ret += repair_item_count_needsrepair(&aitem->sri);

	return ret;
}

/* Add to the list of repairs. */
void
action_list_add(
	struct action_list		*alist,
	struct action_item		*aitem)
{
	list_add_tail(&aitem->list, &alist->list);
	alist->nr++;
	alist->sorted = false;
}

/* Repair everything on this list. */
int
action_list_process(
	struct scrub_ctx		*ctx,
	struct action_list		*alist,
	unsigned int			repair_flags)
{
	struct action_item		*aitem;
	struct action_item		*n;
	int				ret;

	list_for_each_entry_safe(aitem, n, &alist->list, list) {
		if (scrub_excessive_errors(ctx))
			return ECANCELED;

		ret = repair_item(ctx, &aitem->sri, repair_flags);
		if (ret)
			break;

		if (repair_item_count_needsrepair(&aitem->sri) == 0) {
			list_del(&aitem->list);
			free(aitem);
		}
	}

	return ret;
}

/*
 * For a given filesystem object, perform all repairs of a given class
 * (corrupt, xcorrupt, xfail, preen) if the repair item says it's needed.
 */
static int
repair_item_class(
	struct scrub_ctx		*ctx,
	struct scrub_item		*sri,
	int				override_fd,
	uint8_t				repair_mask,
	unsigned int			flags)
{
	struct xfs_fd			xfd;
	struct xfs_fd			*xfdp = &ctx->mnt;
	unsigned int			scrub_type;

	if (ctx->mode < SCRUB_MODE_REPAIR)
		return 0;

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
		enum check_outcome	fix;

		if (scrub_excessive_errors(ctx))
			return ECANCELED;

		if (!(sri->sri_state[scrub_type] & repair_mask))
			continue;

		fix = xfs_repair_metadata(ctx, xfdp, scrub_type, sri, flags);
		switch (fix) {
		case CHECK_DONE:
			if (!(flags & XRM_NOPROGRESS))
				progress_add(1);
			continue;
		case CHECK_ABORT:
			return ECANCELED;
		case CHECK_RETRY:
			continue;
		case CHECK_REPAIR:
			abort();
		}
	}

	return 0;
}

/*
 * Repair all parts (i.e. scrub types) of this filesystem object for which
 * corruption has been observed directly.  Other types of repair work (fixing
 * cross referencing problems and preening) are deferred.
 *
 * This function should only be called to perform spot repairs of fs objects
 * during phase 2 and 3 while we still have open handles to those objects.
 */
int
repair_item_corruption(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri)
{
	return repair_item_class(ctx, sri, -1, SCRUB_ITEM_CORRUPT,
			XRM_REPAIR_ONLY | XRM_NOPROGRESS);
}

/* Repair all parts of this file, similar to repair_item_corruption. */
int
repair_file_corruption(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri,
	int			override_fd)
{
	return repair_item_class(ctx, sri, override_fd, SCRUB_ITEM_CORRUPT,
			XRM_REPAIR_ONLY | XRM_NOPROGRESS);
}

/*
 * Repair everything in this filesystem object that needs it.  This includes
 * cross-referencing and preening.
 */
int
repair_item(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri,
	unsigned int		flags)
{
	int			ret;

	ret = repair_item_class(ctx, sri, -1, SCRUB_ITEM_CORRUPT, flags);
	if (ret)
		return ret;

	ret = repair_item_class(ctx, sri, -1, SCRUB_ITEM_XCORRUPT, flags);
	if (ret)
		return ret;

	ret = repair_item_class(ctx, sri, -1, SCRUB_ITEM_XFAIL, flags);
	if (ret)
		return ret;

	return repair_item_class(ctx, sri, -1, SCRUB_ITEM_PREEN, flags);
}

/* Create an action item around a scrub item that needs repairs. */
int
repair_item_to_action_item(
	struct scrub_ctx	*ctx,
	const struct scrub_item	*sri,
	struct action_item	**aitemp)
{
	struct action_item	*aitem;

	if (repair_item_count_needsrepair(sri) == 0)
		return 0;

	aitem = malloc(sizeof(struct action_item));
	if (!aitem) {
		int		error = errno;

		str_liberror(ctx, error, _("creating repair action item"));
		return error;
	}

	INIT_LIST_HEAD(&aitem->list);
	memcpy(&aitem->sri, sri, sizeof(struct scrub_item));

	*aitemp = aitem;
	return 0;
}

/* Defer all the repairs until phase 4. */
int
repair_item_defer(
	struct scrub_ctx	*ctx,
	const struct scrub_item	*sri)
{
	struct action_item	*aitem = NULL;
	unsigned int		agno;
	int			error;

	error = repair_item_to_action_item(ctx, sri, &aitem);
	if (error || !aitem)
		return error;

	if (sri->sri_agno != -1U)
		agno = sri->sri_agno;
	else if (sri->sri_ino != -1ULL && sri->sri_gen != -1U)
		agno = cvt_ino_to_agno(&ctx->mnt, sri->sri_ino);
	else
		agno = 0;
	ASSERT(agno < ctx->mnt.fsgeom.agcount);

	action_list_add(&ctx->action_lists[agno], aitem);
	return 0;
}
