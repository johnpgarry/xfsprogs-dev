// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef RMAP_H_
#define RMAP_H_

extern bool collect_rmaps;
extern bool rmapbt_suspect;

extern bool rmap_needs_work(struct xfs_mount *);

extern void rmaps_init(struct xfs_mount *);
extern void rmaps_free(struct xfs_mount *);

void rmap_add_rec(struct xfs_mount *mp, xfs_ino_t ino, int whichfork,
		struct xfs_bmbt_irec *irec, bool realtime);
void rmap_add_bmbt_rec(struct xfs_mount *mp, xfs_ino_t ino, int whichfork,
		xfs_fsblock_t fsbno);
bool rmaps_are_mergeable(struct xfs_rmap_irec *r1, struct xfs_rmap_irec *r2);

void rmap_add_fixed_ag_rec(struct xfs_mount *mp, xfs_agnumber_t agno);

int rmap_add_agbtree_mapping(struct xfs_mount *mp, xfs_agnumber_t agno,
		xfs_agblock_t agbno, xfs_extlen_t len, uint64_t owner);
int rmap_commit_agbtree_mappings(struct xfs_mount *mp, xfs_agnumber_t agno);

uint64_t rmap_record_count(struct xfs_mount *mp, bool isrt,
		xfs_agnumber_t agno);
extern void rmap_avoid_check(void);
void rmaps_verify_btree(struct xfs_mount *mp, xfs_agnumber_t agno);

extern int64_t rmap_diffkeys(struct xfs_rmap_irec *kp1,
		struct xfs_rmap_irec *kp2);
extern void rmap_high_key_from_rec(struct xfs_rmap_irec *rec,
		struct xfs_rmap_irec *key);

extern int compute_refcounts(struct xfs_mount *, xfs_agnumber_t);
uint64_t refcount_record_count(struct xfs_mount *mp, xfs_agnumber_t agno);
extern int init_refcount_cursor(xfs_agnumber_t, struct xfs_slab_cursor **);
extern void refcount_avoid_check(void);
void check_refcounts(struct xfs_mount *mp, xfs_agnumber_t agno);

extern void record_inode_reflink_flag(struct xfs_mount *, struct xfs_dinode *,
	xfs_agnumber_t, xfs_agino_t, xfs_ino_t);
extern int fix_inode_reflink_flags(struct xfs_mount *, xfs_agnumber_t);

extern void fix_freelist(struct xfs_mount *, xfs_agnumber_t, bool);
extern void rmap_store_agflcount(struct xfs_mount *, xfs_agnumber_t, int);

xfs_extlen_t estimate_rmapbt_blocks(struct xfs_perag *pag);
xfs_extlen_t estimate_refcountbt_blocks(struct xfs_perag *pag);

int rmap_init_mem_cursor(struct xfs_mount *mp, struct xfs_trans *tp,
		bool isrt, xfs_agnumber_t agno, struct xfs_btree_cur **rmcurp);
int rmap_get_mem_rec(struct xfs_btree_cur *rmcur, struct xfs_rmap_irec *irec);

#endif /* RMAP_H_ */
