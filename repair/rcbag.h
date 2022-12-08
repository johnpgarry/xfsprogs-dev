// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __RCBAG_H__
#define __RCBAG_H__

struct xfs_mount;
struct rcbag;

int rcbag_init(struct xfs_mount *mp, uint64_t max_rmaps, struct rcbag **bagp);
void rcbag_free(struct rcbag **bagp);
void rcbag_add(struct rcbag *bag, const struct xfs_rmap_irec *rmap);
uint64_t rcbag_count(const struct rcbag *bag);

void rcbag_next_edge(struct rcbag *bag, const struct xfs_rmap_irec *next_rmap,
		bool next_valid, uint32_t *next_bnop);
void rcbag_remove_ending_at(struct rcbag *bag, uint32_t next_bno);

struct rcbag_iter {
	struct xfs_btree_cur	*cur;
	uint64_t		ino;
};

void rcbag_ino_iter_start(struct rcbag *bag, struct rcbag_iter *iter);
void rcbag_ino_iter_stop(struct rcbag *bag, struct rcbag_iter *iter);
int rcbag_ino_iter(struct rcbag *bag, struct rcbag_iter *iter);

void rcbag_dump(struct rcbag *bag);

#endif /* __RCBAG_H__ */
