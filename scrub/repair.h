/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_REPAIR_H_
#define XFS_SCRUB_REPAIR_H_

struct action_list {
	struct list_head	list;
};

struct action_item;

int action_list_alloc(struct action_list **listp);
void action_list_free(struct action_list **listp);
static inline void action_list_init(struct action_list *alist)
{
	INIT_LIST_HEAD(&alist->list);
}

unsigned long long action_list_length(struct action_list *alist);

/* Move all the items of @src to the tail of @dst, and reinitialize @src. */
static inline void
action_list_merge(
	struct action_list	*dst,
	struct action_list	*src)
{
	list_splice_tail_init(&src->list, &dst->list);
}

struct action_item *action_list_pop(struct action_list *alist);
void action_list_add(struct action_list *alist, struct action_item *aitem);

static inline bool action_list_empty(const struct action_list *alist)
{
	return list_empty(&alist->list);
}

enum tryrepair_outcome {
	/* No progress was made on repairs at all. */
	TR_NOPROGRESS = 0,
	/* Some progress was made on repairs; try again soon. */
	TR_REQUEUE,
	/* Repairs completely successful. */
	TR_REPAIRED,
};

int action_item_try_repair(struct scrub_ctx *ctx, struct action_item *aitem,
		enum tryrepair_outcome *outcome);

void repair_item_mustfix(struct scrub_item *sri, struct scrub_item *fix_now);

/* Primary metadata is corrupt */
#define REPAIR_DIFFICULTY_PRIMARY	(1U << 0)
/* Secondary metadata is corrupt */
#define REPAIR_DIFFICULTY_SECONDARY	(1U << 1)
unsigned int repair_item_difficulty(const struct scrub_item *sri);

/*
 * Only ask the kernel to repair this object if the kernel directly told us it
 * was corrupt.  Objects that are only flagged as having cross-referencing
 * errors or flagged as eligible for optimization are left for later.
 */
#define XRM_REPAIR_ONLY		(1U << 0)

/* This is the last repair attempt; complain if still broken even after fix. */
#define XRM_FINAL_WARNING	(1U << 1)

/* Don't call progress_add after repairing an item. */
#define XRM_NOPROGRESS		(1U << 2)

int action_list_process(struct scrub_ctx *ctx, struct action_list *alist,
		unsigned int repair_flags);
int repair_item_corruption(struct scrub_ctx *ctx, struct scrub_item *sri);
int repair_file_corruption(struct scrub_ctx *ctx, struct scrub_item *sri,
		int override_fd);
int repair_file_corruption_now(struct scrub_ctx *ctx, struct scrub_item *sri,
		int override_fd);
int repair_item(struct scrub_ctx *ctx, struct scrub_item *sri,
		unsigned int repair_flags);
int repair_item_to_action_item(struct scrub_ctx *ctx,
		const struct scrub_item *sri, struct action_item **aitemp);

static inline unsigned int
repair_item_count_needsrepair(
	const struct scrub_item	*sri)
{
	unsigned int		scrub_type;
	unsigned int		nr = 0;

	foreach_scrub_type(scrub_type)
		if (sri->sri_state[scrub_type] & SCRUB_ITEM_REPAIR_ANY)
			nr++;
	return nr;
}

static inline int
repair_item_completely(
	struct scrub_ctx	*ctx,
	struct scrub_item	*sri)
{
	return repair_item(ctx, sri, XRM_FINAL_WARNING | XRM_NOPROGRESS);
}

bool repair_want_service_downgrade(struct scrub_ctx *ctx);

#endif /* XFS_SCRUB_REPAIR_H_ */
