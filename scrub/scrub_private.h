// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_SCRUB_PRIVATE_H_
#define XFS_SCRUB_SCRUB_PRIVATE_H_

/* Shared code between scrub.c and repair.c. */

/*
 * Declare a structure big enough to handle all scrub types + barriers, and
 * an iteration pointer.  So far we only need two barriers.
 */
struct scrubv_head {
	struct xfs_scrub_vec_head	head;
	struct xfs_scrub_vec		__vecs[XFS_SCRUB_TYPE_NR + 2];
	unsigned int			i;
};

#define foreach_bighead_vec(bh, v) \
	for ((bh)->i = 0, (v) = (bh)->head.svh_vecs; \
	     (bh)->i < (bh)->head.svh_nr; \
	     (bh)->i++, (v)++)

void scrub_item_to_vhead(struct scrubv_head *bighead,
		const struct scrub_item *sri);
void scrub_vhead_add(struct scrubv_head *bighead, const struct scrub_item *sri,
		unsigned int scrub_type, bool repair);
void scrub_vhead_add_barrier(struct scrubv_head *bighead);

int format_scrubv_descr(struct scrub_ctx *ctx, char *buf, size_t buflen,
		void *where);

/* Predicates for scrub flag state. */

static inline bool is_corrupt(const struct xfs_scrub_vec *sv)
{
	return sv->sv_flags & XFS_SCRUB_OFLAG_CORRUPT;
}

static inline bool is_unoptimized(const struct xfs_scrub_vec *sv)
{
	return sv->sv_flags & XFS_SCRUB_OFLAG_PREEN;
}

static inline bool xref_failed(const struct xfs_scrub_vec *sv)
{
	return sv->sv_flags & XFS_SCRUB_OFLAG_XFAIL;
}

static inline bool xref_disagrees(const struct xfs_scrub_vec *sv)
{
	return sv->sv_flags & XFS_SCRUB_OFLAG_XCORRUPT;
}

static inline bool is_incomplete(const struct xfs_scrub_vec *sv)
{
	return sv->sv_flags & XFS_SCRUB_OFLAG_INCOMPLETE;
}

static inline bool is_suspicious(const struct xfs_scrub_vec *sv)
{
	return sv->sv_flags & XFS_SCRUB_OFLAG_WARNING;
}

/* Should we fix it? */
static inline bool needs_repair(const struct xfs_scrub_vec *sv)
{
	return is_corrupt(sv) || xref_disagrees(sv);
}

/*
 * We want to retry an operation if the kernel says it couldn't complete the
 * scan/repair; or if there were cross-referencing problems but the object was
 * not obviously corrupt.
 */
static inline bool want_retry(const struct xfs_scrub_vec *sv)
{
	return is_incomplete(sv) || (xref_disagrees(sv) && !is_corrupt(sv));
}

void scrub_warn_incomplete_scrub(struct scrub_ctx *ctx, struct descr *dsc,
		const struct xfs_scrub_vec *meta);

/* Scrub item functions */

static inline void
scrub_item_save_state(
	struct scrub_item		*sri,
	unsigned  int			scrub_type,
	unsigned  int			scrub_flags)
{
	sri->sri_state[scrub_type] = scrub_flags & SCRUB_ITEM_REPAIR_ANY;
	if (scrub_flags & SCRUB_ITEM_NEEDSREPAIR)
		sri->sri_inconsistent = true;
}

static inline void
scrub_item_clean_state(
	struct scrub_item		*sri,
	unsigned  int			scrub_type)
{
	sri->sri_state[scrub_type] = 0;
}

static inline bool
scrub_item_type_boosted(
	struct scrub_item		*sri,
	unsigned  int			scrub_type)
{
	return sri->sri_state[scrub_type] & SCRUB_ITEM_BOOST_REPAIR;
}

/* Decide if we want to retry this operation and update bookkeeping if yes. */
static inline bool
scrub_item_schedule_retry(struct scrub_item *sri, unsigned int scrub_type)
{
	if (sri->sri_tries[scrub_type] == 0)
		return false;
	sri->sri_tries[scrub_type]--;
	return true;
}

bool scrub_item_call_kernel_again(struct scrub_item *sri, uint8_t work_mask,
		const struct scrub_item *old);
bool scrub_item_schedule_work(struct scrub_item *sri, uint8_t state_flags,
		const unsigned int *schedule_deps);

#endif /* XFS_SCRUB_SCRUB_PRIVATE_H_ */
