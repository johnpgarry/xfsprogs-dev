/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_REPAIR_H_
#define XFS_SCRUB_REPAIR_H_

struct action_list {
	struct list_head	list;
	unsigned long long	nr;
	bool			sorted;
};

int action_lists_alloc(size_t nr, struct action_list **listsp);
void action_lists_free(struct action_list **listsp);

void action_list_init(struct action_list *alist);

static inline bool action_list_empty(const struct action_list *alist)
{
	return list_empty(&alist->list);
}

unsigned long long action_list_length(struct action_list *alist);
void action_list_add(struct action_list *dest, struct action_item *item);
void action_list_discard(struct action_list *alist);
void action_list_splice(struct action_list *dest, struct action_list *src);

void action_list_find_mustfix(struct action_list *actions,
		struct action_list *immediate_alist);

/* Primary metadata is corrupt */
#define REPAIR_DIFFICULTY_PRIMARY	(1U << 0)
/* Secondary metadata is corrupt */
#define REPAIR_DIFFICULTY_SECONDARY	(1U << 1)
unsigned int action_list_difficulty(const struct action_list *actions);

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

int action_list_process(struct scrub_ctx *ctx, int fd,
		struct action_list *alist, unsigned int repair_flags);
void action_list_defer(struct scrub_ctx *ctx, xfs_agnumber_t agno,
		struct action_list *alist);
int action_list_process_or_defer(struct scrub_ctx *ctx, xfs_agnumber_t agno,
		struct action_list *alist);

#endif /* XFS_SCRUB_REPAIR_H_ */
