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

/*
 * Bitmap showing the correctness dependencies between scrub types for repairs.
 * There are no edges between AG btrees and AG headers because we can't mount
 * the filesystem if the btree root pointers in the AG headers are wrong.
 * Dependencies cannot cross scrub groups.
 */
#define DEP(x) (1U << (x))
static const unsigned int repair_deps[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_BMBTD]		= DEP(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_BMBTA]		= DEP(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_BMBTC]		= DEP(XFS_SCRUB_TYPE_INODE),
	[XFS_SCRUB_TYPE_DIR]		= DEP(XFS_SCRUB_TYPE_BMBTD),
	[XFS_SCRUB_TYPE_XATTR]		= DEP(XFS_SCRUB_TYPE_BMBTA),
	[XFS_SCRUB_TYPE_SYMLINK]	= DEP(XFS_SCRUB_TYPE_BMBTD),
	[XFS_SCRUB_TYPE_PARENT]		= DEP(XFS_SCRUB_TYPE_DIR) |
					  DEP(XFS_SCRUB_TYPE_XATTR),
	[XFS_SCRUB_TYPE_QUOTACHECK]	= DEP(XFS_SCRUB_TYPE_UQUOTA) |
					  DEP(XFS_SCRUB_TYPE_GQUOTA) |
					  DEP(XFS_SCRUB_TYPE_PQUOTA),
	[XFS_SCRUB_TYPE_RTSUM]		= DEP(XFS_SCRUB_TYPE_RTBITMAP),
};
#undef DEP

/*
 * Decide if we want an automatic downgrade to dry-run mode.  This is only
 * for service mode, where we are fed a path and have to figure out if the fs
 * is repairable or not.
 */
bool
repair_want_service_downgrade(
	struct scrub_ctx		*ctx)
{
	struct xfs_scrub_metadata	meta = {
		.sm_type		= XFS_SCRUB_TYPE_PROBE,
		.sm_flags		= XFS_SCRUB_IFLAG_REPAIR,
	};
	int				error;

	if (ctx->mode == SCRUB_MODE_DRY_RUN)
		return false;
	if (!is_service)
		return false;
	if (debug_tweak_on("XFS_SCRUB_NO_KERNEL"))
		return false;

	error = -xfrog_scrub_metadata(&ctx->mnt, &meta);
	switch (error) {
	case EROFS:
	case ENOTRECOVERABLE:
	case EOPNOTSUPP:
		return true;
	}

	return false;
}

static inline void
restore_oldvec(
	struct xfs_scrub_vec	*oldvec,
	const struct scrub_item	*sri,
	unsigned int		scrub_type)
{
	oldvec->sv_type = scrub_type;
	oldvec->sv_flags = sri->sri_state[scrub_type] & SCRUB_ITEM_REPAIR_ANY;
}

static int
repair_epilogue(
	struct scrub_ctx		*ctx,
	struct descr			*dsc,
	struct scrub_item		*sri,
	unsigned int			repair_flags,
	const struct xfs_scrub_vec	*meta)
{
	struct xfs_scrub_vec		oldv;
	struct xfs_scrub_vec		*oldm = &oldv;
	unsigned int			scrub_type = meta->sv_type;
	int				error = -meta->sv_ret;

	restore_oldvec(oldm, sri, meta->sv_type);

	switch (error) {
	case 0:
		/* No operational errors encountered. */
		break;
	case EDEADLOCK:
	case EBUSY:
		/* Filesystem is busy, try again later. */
		if (debug || verbose)
			str_info(ctx, descr_render(dsc),
_("Filesystem is busy, deferring repair."));
		return 0;
	case ESHUTDOWN:
		/* Filesystem is already shut down, abort. */
		str_error(ctx, descr_render(dsc),
_("Filesystem is shut down, aborting."));
		return ECANCELED;
	case ENOTTY:
	case EOPNOTSUPP:
		/*
		 * If the kernel cannot perform the optimization that we
		 * requested; or we forced a repair but the kernel doesn't know
		 * how to perform the repair, don't requeue the request.  Mark
		 * it done and move on.
		 */
		if (is_unoptimized(oldm) ||
		    debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
			scrub_item_clean_state(sri, scrub_type);
			return 0;
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
			return 0;
		fallthrough;
	case EINVAL:
		/* Kernel doesn't know how to repair this? */
		str_corrupt(ctx, descr_render(dsc),
_("Don't know how to fix; offline repair required."));
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	case EROFS:
		/* Read-only filesystem, can't fix. */
		if (verbose || debug || needs_repair(oldm))
			str_error(ctx, descr_render(dsc),
_("Read-only filesystem; cannot make changes."));
		return ECANCELED;
	case ENOENT:
		/* Metadata not present, just skip it. */
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	case ENOMEM:
	case ENOSPC:
		/* Don't care if preen fails due to low resources. */
		if (is_unoptimized(oldm) && !needs_repair(oldm)) {
			scrub_item_clean_state(sri, scrub_type);
			return 0;
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
			return 0;
		str_liberror(ctx, error, descr_render(dsc));
		scrub_item_clean_state(sri, scrub_type);
		return 0;
	}

	/*
	 * If the kernel says the repair was incomplete or that there was a
	 * cross-referencing discrepancy but no obvious corruption, we'll try
	 * the repair again, just in case the fs was busy.  Only retry so many
	 * times.
	 */
	if (want_retry(meta) && scrub_item_schedule_retry(sri, scrub_type))
		return 0;

	if (repair_flags & XRM_FINAL_WARNING)
		scrub_warn_incomplete_scrub(ctx, dsc, meta);
	if (needs_repair(meta) || is_incomplete(meta)) {
		/*
		 * Still broken; if we've been told not to complain then we
		 * just requeue this and try again later.  Otherwise we
		 * log the error loudly and don't try again.
		 */
		if (!(repair_flags & XRM_FINAL_WARNING))
			return 0;
		str_corrupt(ctx, descr_render(dsc),
_("Repair unsuccessful; offline repair required."));
	} else if (xref_failed(meta)) {
		/*
		 * This metadata object itself looks ok, but we still noticed
		 * inconsistencies when comparing it with the other filesystem
		 * metadata.  If we're in "final warning" mode, advise the
		 * caller to run xfs_repair; otherwise, we'll keep trying to
		 * reverify the cross-referencing as repairs progress.
		 */
		if (repair_flags & XRM_FINAL_WARNING) {
			str_info(ctx, descr_render(dsc),
 _("Seems correct but cross-referencing failed; offline repair recommended."));
		} else {
			if (verbose)
				str_info(ctx, descr_render(dsc),
 _("Seems correct but cross-referencing failed; will keep checking."));
			return 0;
		}
	} else if (meta->sv_flags & XFS_SCRUB_OFLAG_NO_REPAIR_NEEDED) {
		if (verbose)
			str_info(ctx, descr_render(dsc),
					_("No modification needed."));
	} else {
		/* Clean operation, no corruption detected. */
		if (is_corrupt(oldm))
			record_repair(ctx, descr_render(dsc),
 _("Repairs successful."));
		else if (xref_disagrees(oldm))
			record_repair(ctx, descr_render(dsc),
 _("Repairs successful after discrepancy in cross-referencing."));
		else if (xref_failed(oldm))
			record_repair(ctx, descr_render(dsc),
 _("Repairs successful after cross-referencing failure."));
		else
			record_preen(ctx, descr_render(dsc),
 _("Optimization successful."));
	}

	scrub_item_clean_state(sri, scrub_type);
	return 0;
}

/* Decide if the dependent scrub types of the given scrub type are ok. */
static bool
repair_item_dependencies_ok(
	const struct scrub_item	*sri,
	unsigned int		scrub_type)
{
	unsigned int		dep_mask = repair_deps[scrub_type];
	unsigned int		b;

	for (b = 0; dep_mask && b < XFS_SCRUB_TYPE_NR; b++, dep_mask >>= 1) {
		if (!(dep_mask & 1))
			continue;
		/*
		 * If this lower level object also needs repair, we can't fix
		 * the higher level item.
		 */
		if (sri->sri_state[b] & SCRUB_ITEM_NEEDSREPAIR)
			return false;
	}

	return true;
}

/* Decide if we want to repair a particular type of metadata. */
static bool
can_repair_now(
	const struct scrub_item	*sri,
	unsigned int		scrub_type,
	__u32			repair_mask,
	unsigned int		repair_flags)
{
	struct xfs_scrub_vec	oldvec;
	bool			repair_only;

	/* Do we even need to repair this thing? */
	if (!(sri->sri_state[scrub_type] & repair_mask))
		return false;

	restore_oldvec(&oldvec, sri, scrub_type);

	/*
	 * If the caller boosted the priority of this scrub type on behalf of a
	 * higher level repair by setting IFLAG_REPAIR, ignore REPAIR_ONLY.
	 */
	repair_only = (repair_flags & XRM_REPAIR_ONLY) &&
		      !(sri->sri_state[scrub_type] & SCRUB_ITEM_BOOST_REPAIR);
	if (!is_corrupt(&oldvec) && repair_only)
		return false;

	/*
	 * Don't try to repair higher level items if their lower-level
	 * dependencies haven't been verified, unless this is our last chance
	 * to fix things without complaint.
	 */
	if (!(repair_flags & XRM_FINAL_WARNING) &&
	    !repair_item_dependencies_ok(sri, scrub_type))
		return false;

	return true;
}

/*
 * Repair some metadata.
 *
 * Returns 0 for success (or repair item deferral), or ECANCELED to abort the
 * program.
 */
static int
repair_call_kernel(
	struct scrub_ctx		*ctx,
	struct xfs_fd			*xfdp,
	struct scrub_item		*sri,
	__u32				repair_mask,
	unsigned int			repair_flags)
{
	DEFINE_DESCR(dsc, ctx, format_scrubv_descr);
	struct scrubv_head		bh = { };
	struct xfs_scrub_vec		*v;
	unsigned int			scrub_type;
	bool				need_barrier = false;
	int				error;

	assert(!debug_tweak_on("XFS_SCRUB_NO_KERNEL"));

	scrub_item_to_vhead(&bh, sri);
	descr_set(&dsc, &bh);

	foreach_scrub_type(scrub_type) {
		if (scrub_excessive_errors(ctx))
			return ECANCELED;

		if (!can_repair_now(sri, scrub_type, repair_mask,
					repair_flags))
			continue;

		if (need_barrier) {
			scrub_vhead_add_barrier(&bh);
			need_barrier = false;
		}

		scrub_vhead_add(&bh, sri, scrub_type, true);

		if (sri->sri_state[scrub_type] & SCRUB_ITEM_NEEDSREPAIR)
			str_info(ctx, descr_render(&dsc),
					_("Attempting repair."));
		else if (debug || verbose)
			str_info(ctx, descr_render(&dsc),
					_("Attempting optimization."));

		dbg_printf("repair %s flags %xh tries %u\n", descr_render(&dsc),
				sri->sri_state[scrub_type],
				sri->sri_tries[scrub_type]);

		/*
		 * One of the other scrub types depends on this one.  Set us up
		 * to add a repair barrier if we decide to schedule a repair
		 * after this one.  If the UNFIXED flag is set, that means this
		 * is our last chance to fix things, so we skip the barriers
		 * just let everything run.
		 */
		if (!(repair_flags & XRM_FINAL_WARNING) &&
		    (sri->sri_state[scrub_type] & SCRUB_ITEM_BARRIER))
			need_barrier = true;
	}

	error = -xfrog_scrubv_metadata(xfdp, &bh.head);
	if (error)
		return error;

	foreach_bighead_vec(&bh, v) {
		/* Deal with barriers separately. */
		if (v->sv_type == XFS_SCRUB_TYPE_BARRIER) {
			/* -ECANCELED means the kernel stopped here. */
			if (v->sv_ret == -ECANCELED)
				return 0;
			if (v->sv_ret)
				return -v->sv_ret;
			continue;
		}

		error = repair_epilogue(ctx, &dsc, sri, repair_flags, v);
		if (error)
			return error;

		/* Maybe update progress if we fixed the problem. */
		if (!(repair_flags & XRM_NOPROGRESS) &&
		    !(sri->sri_state[v->sv_type] & SCRUB_ITEM_REPAIR_ANY))
			progress_add(1);
	}

	return 0;
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
 * The operation of higher level metadata objects depends on the correctness of
 * lower level metadata objects.  This means that if X depends on Y, we must
 * investigate and correct all the observed issues with Y before we try to make
 * a correction to X.  For all scheduled repair activity on X, boost the
 * priority of repairs on all the Ys to ensure this correctness.
 */
static void
repair_item_boost_priorities(
	struct scrub_item		*sri)
{
	unsigned int			scrub_type;

	foreach_scrub_type(scrub_type) {
		unsigned int		dep_mask = repair_deps[scrub_type];
		unsigned int		b;

		if (repair_item_count_needsrepair(sri) == 0 || !dep_mask)
			continue;

		/*
		 * Check if the repairs for this scrub type depend on any other
		 * scrub types that have been flagged with cross-referencing
		 * errors and are not already tagged for the highest priority
		 * repair (SCRUB_ITEM_CORRUPT).  If so, boost the priority of
		 * that scrub type (via SCRUB_ITEM_BOOST_REPAIR) so that any
		 * problems with the dependencies will (hopefully) be fixed
		 * before we start repairs on this scrub type.
		 *
		 * So far in the history of xfs_scrub we have maintained that
		 * lower numbered scrub types do not depend on higher numbered
		 * scrub types, so we need only process the bit mask once.
		 */
		for (b = 0; b < XFS_SCRUB_TYPE_NR; b++, dep_mask >>= 1) {
			if (!dep_mask)
				break;
			if (!(dep_mask & 1))
				continue;
			if (!(sri->sri_state[b] & SCRUB_ITEM_REPAIR_XREF))
				continue;
			if (sri->sri_state[b] & SCRUB_ITEM_CORRUPT)
				continue;
			sri->sri_state[b] |= SCRUB_ITEM_BOOST_REPAIR;
		}
	}
}

/*
 * These are the scrub item state bits that must be copied when scheduling
 * a (per-AG) scrub type for immediate repairs.  The original state tracking
 * bits are left untouched to force a rescan in phase 4.
 */
#define MUSTFIX_STATES	(SCRUB_ITEM_CORRUPT | \
			 SCRUB_ITEM_BOOST_REPAIR | \
			 SCRUB_ITEM_BARRIER)
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
	repair_item_boost_priorities(sri);
	scrub_item_init_ag(fix_now, sri->sri_agno);

	foreach_scrub_type(scrub_type) {
		unsigned int	state;

		state = sri->sri_state[scrub_type] & MUSTFIX_STATES;
		if (!state)
			continue;

		switch (scrub_type) {
		case XFS_SCRUB_TYPE_AGI:
		case XFS_SCRUB_TYPE_FINOBT:
		case XFS_SCRUB_TYPE_INOBT:
			fix_now->sri_state[scrub_type] = state;
			break;
		}
	}
}

/*
 * These scrub item states correspond to metadata that is inconsistent in some
 * way and must be repaired.  If too many metadata objects share these states,
 * this can make repairs difficult.
 */
#define HARDREPAIR_STATES	(SCRUB_ITEM_CORRUPT | \
				 SCRUB_ITEM_XCORRUPT | \
				 SCRUB_ITEM_XFAIL)

/* Determine if primary or secondary metadata are inconsistent. */
unsigned int
repair_item_difficulty(
	const struct scrub_item	*sri)
{
	unsigned int		scrub_type;
	unsigned int		ret = 0;

	foreach_scrub_type(scrub_type) {
		unsigned int	state;

		state = sri->sri_state[scrub_type] & HARDREPAIR_STATES;
		if (!state)
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
		case XFS_SCRUB_TYPE_RGSUPER:
			ret |= REPAIR_DIFFICULTY_PRIMARY;
			break;
		}
	}

	return ret;
}

/* Create a new repair action list. */
int
action_list_alloc(
	struct action_list		**listp)
{
	struct action_list		*alist;

	alist = malloc(sizeof(struct action_list));
	if (!alist)
		return errno;

	action_list_init(alist);
	*listp = alist;
	return 0;
}

/* Free the repair lists. */
void
action_list_free(
	struct action_list		**listp)
{
	struct action_list		*alist = *listp;
	struct action_item		*aitem;
	struct action_item		*n;

	if (!(*listp))
		return;

	list_for_each_entry_safe(aitem, n, &alist->list, list) {
		list_del(&aitem->list);
		free(aitem);
	}

	free(alist);
	*listp = NULL;
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

/* Remove the first action item from the action list. */
struct action_item *
action_list_pop(
	struct action_list		*alist)
{
	struct action_item		*aitem;

	aitem = list_first_entry_or_null(&alist->list, struct action_item,
			list);
	if (!aitem)
		return NULL;

	list_del_init(&aitem->list);
	return aitem;
}

/* Add an action item to the end of a list. */
void
action_list_add(
	struct action_list		*alist,
	struct action_item		*aitem)
{
	list_add_tail(&aitem->list, &alist->list);
}

/*
 * Try to repair a filesystem object and let the caller know what it should do
 * with the action item.  The caller must be able to requeue action items, so
 * we don't complain if repairs are not totally successful.
 */
int
action_item_try_repair(
	struct scrub_ctx	*ctx,
	struct action_item	*aitem,
	enum tryrepair_outcome	*outcome)
{
	struct scrub_item	*sri = &aitem->sri;
	unsigned int		before, after;
	unsigned int		scrub_type;
	int			ret;

	BUILD_BUG_ON(sizeof(sri->sri_selected) * NBBY < XFS_SCRUB_TYPE_NR);
	before = repair_item_count_needsrepair(sri);

	ret = repair_item(ctx, sri, 0);
	if (ret)
		return ret;

	after = repair_item_count_needsrepair(sri);
	if (after > 0) {
		/*
		 * The kernel did not complete all of the repairs requested.
		 * If it made some progress we'll requeue; otherwise, let the
		 * caller know that nothing got fixed.
		 */
		if (before != after)
			*outcome = TR_REQUEUE;
		else
			*outcome = TR_NOPROGRESS;
		return 0;
	}

	/*
	 * Nothing in this fs object was marked inconsistent.  This means we
	 * were merely optimizing metadata and there is no revalidation work to
	 * be done.
	 */
	if (!sri->sri_inconsistent) {
		*outcome = TR_REPAIRED;
		return 0;
	}

	/*
	 * We fixed inconsistent metadata, so reschedule the entire object for
	 * immediate revalidation to see if anything else went wrong.
	 */
	foreach_scrub_type(scrub_type)
		if (sri->sri_selected & (1ULL << scrub_type))
			sri->sri_state[scrub_type] = SCRUB_ITEM_NEEDSCHECK;
	sri->sri_inconsistent = false;
	sri->sri_revalidate = true;

	ret = scrub_item_check(ctx, sri);
	if (ret)
		return ret;

	after = repair_item_count_needsrepair(sri);
	if (after > 0) {
		/*
		 * Uhoh, we found something else broken.  Tell the caller that
		 * this item needs to be queued for more repairs.
		 */
		sri->sri_revalidate = false;
		*outcome = TR_REQUEUE;
		return 0;
	}

	/* Repairs complete. */
	*outcome = TR_REPAIRED;
	return 0;
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
	struct scrub_item		old_sri;
	struct xfs_fd			*xfdp = &ctx->mnt;
	int				error = 0;

	if (ctx->mode == SCRUB_MODE_DRY_RUN)
		return 0;
	if (ctx->mode == SCRUB_MODE_PREEN && !(repair_mask & SCRUB_ITEM_PREEN))
		return 0;
	if (!scrub_item_schedule_work(sri, repair_mask, repair_deps))
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

	do {
		memcpy(&old_sri, sri, sizeof(struct scrub_item));
		error = repair_call_kernel(ctx, xfdp, sri, repair_mask, flags);
		if (error)
			return error;
	} while (scrub_item_call_kernel_again(sri, repair_mask, &old_sri));

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
	repair_item_boost_priorities(sri);

	return repair_item_class(ctx, sri, override_fd, SCRUB_ITEM_CORRUPT,
			XRM_REPAIR_ONLY | XRM_NOPROGRESS);
}

/* Repair all parts of this file or complain if we cannot. */
int
repair_file_corruption_now(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri,
	int			override_fd)
{
	repair_item_boost_priorities(sri);

	return repair_item_class(ctx, sri, override_fd, SCRUB_ITEM_CORRUPT,
			XRM_REPAIR_ONLY | XRM_NOPROGRESS | XRM_FINAL_WARNING);
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

	repair_item_boost_priorities(sri);

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
	unsigned int		scrub_type;

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

	/*
	 * If the scrub item indicates that there is unchecked metadata, assume
	 * that the scrub type checker depends on something that couldn't be
	 * fixed.  Mark that type as corrupt so that phase 4 will try it again.
	 */
	foreach_scrub_type(scrub_type) {
		__u8		*state = aitem->sri.sri_state;

		if (state[scrub_type] & SCRUB_ITEM_NEEDSCHECK) {
			state[scrub_type] &= ~SCRUB_ITEM_NEEDSCHECK;
			state[scrub_type] |= SCRUB_ITEM_CORRUPT;
		}
	}

	*aitemp = aitem;
	return 0;
}
