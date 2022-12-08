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

#endif /* SLAB_H_ */
