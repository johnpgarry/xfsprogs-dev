/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __XFS_REPAIR_AG_BTREE_H__
#define __XFS_REPAIR_AG_BTREE_H__

/* Context for rebuilding a per-AG btree. */
struct bt_rebuild {
	/* Fake root for staging and space preallocations. */
	struct bulkload	newbt;

	/* Geometry of the new btree. */
	struct xfs_btree_bload	bload;

	/* Staging btree cursor for the new tree. */
	struct xfs_btree_cur	*cur;

	/* Tree-specific data. */
	union {
		struct xfs_slab_cursor	*slab_cursor;
		struct {
			struct extent_tree_node	*bno_rec;
			unsigned int		freeblks;
		};
	};
};

void finish_rebuild(struct xfs_mount *mp, struct bt_rebuild *btr,
		struct xfs_slab *lost_fsb);
void init_freespace_cursors(struct repair_ctx *sc, xfs_agnumber_t agno,
		unsigned int free_space, unsigned int *nr_extents,
		int *extra_blocks, struct bt_rebuild *btr_bno,
		struct bt_rebuild *btr_cnt);
void build_freespace_btrees(struct repair_ctx *sc, xfs_agnumber_t agno,
		struct bt_rebuild *btr_bno, struct bt_rebuild *btr_cnt);

#endif /* __XFS_REPAIR_AG_BTREE_H__ */
