// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef SLAB_H_
#define SLAB_H_

struct xfs_slab;
struct xfs_slab_cursor;

int init_slab(struct xfs_slab **slabp, size_t item_sz);
void free_slab(struct xfs_slab **slabp);

int slab_add(struct xfs_slab *slab, void *item);
void qsort_slab(struct xfs_slab *slab,
		int (*compare)(const void *, const void *));
uint64_t slab_count(struct xfs_slab *slab);

int init_slab_cursor(struct xfs_slab *slab,
		int (*compare)(const void *, const void *),
		struct xfs_slab_cursor **curp);
void free_slab_cursor(struct xfs_slab_cursor **curp);

void *peek_slab_cursor(struct xfs_slab_cursor *cur);
void advance_slab_cursor(struct xfs_slab_cursor *cur);
void *pop_slab_cursor(struct xfs_slab_cursor *cur);

struct xfs_bag;

int init_bag(struct xfs_bag **bagp);
void free_bag(struct xfs_bag **bagp);
int bag_add(struct xfs_bag *bag, void *item);
int bag_remove(struct xfs_bag *bag, uint64_t idx);
uint64_t bag_count(struct xfs_bag *bag);
void *bag_item(struct xfs_bag *bag, uint64_t idx);

#define foreach_bag_ptr(bag, idx, ptr) \
	for ((idx) = 0, (ptr) = bag_item((bag), (idx)); \
	     (idx) < bag_count(bag); \
	     (idx)++, (ptr) = bag_item((bag), (idx)))

#define foreach_bag_ptr_reverse(bag, idx, ptr) \
	for ((idx) = bag_count(bag) - 1, (ptr) = bag_item((bag), (idx)); \
	     (ptr) != NULL; \
	     (idx)--, (ptr) = bag_item((bag), (idx)))

#endif /* SLAB_H_ */
