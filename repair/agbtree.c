// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <libxfs.h>
#include "err_protos.h"
#include "slab.h"
#include "rmap.h"
#include "incore.h"
#include "bulkload.h"
#include "agbtree.h"

/* Initialize a btree rebuild context. */
static void
init_rebuild(
	struct repair_ctx		*sc,
	const struct xfs_owner_info	*oinfo,
	xfs_agblock_t			free_space,
	struct bt_rebuild		*btr)
{
	memset(btr, 0, sizeof(struct bt_rebuild));

	bulkload_init_ag(&btr->newbt, sc, oinfo);
	bulkload_estimate_ag_slack(sc, &btr->bload, free_space);
}

/*
 * Update this free space record to reflect the blocks we stole from the
 * beginning of the record.
 */
static void
consume_freespace(
	xfs_agnumber_t		agno,
	struct extent_tree_node	*ext_ptr,
	uint32_t		len)
{
	struct extent_tree_node	*bno_ext_ptr;
	xfs_agblock_t		new_start = ext_ptr->ex_startblock + len;
	xfs_extlen_t		new_len = ext_ptr->ex_blockcount - len;

	/* Delete the used-up extent from both extent trees. */
#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "releasing extent: %u [%u %u]\n", agno,
			ext_ptr->ex_startblock, ext_ptr->ex_blockcount);
#endif
	bno_ext_ptr = find_bno_extent(agno, ext_ptr->ex_startblock);
	ASSERT(bno_ext_ptr != NULL);
	get_bno_extent(agno, bno_ext_ptr);
	release_extent_tree_node(bno_ext_ptr);

	ext_ptr = get_bcnt_extent(agno, ext_ptr->ex_startblock,
			ext_ptr->ex_blockcount);
	release_extent_tree_node(ext_ptr);

	/*
	 * If we only used part of this last extent, then we must reinsert the
	 * extent to maintain proper sorting order.
	 */
	if (new_len > 0) {
		add_bno_extent(agno, new_start, new_len);
		add_bcnt_extent(agno, new_start, new_len);
	}
}

/* Reserve blocks for the new per-AG structures. */
static void
reserve_btblocks(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr,
	uint32_t		nr_blocks)
{
	struct extent_tree_node	*ext_ptr;
	uint32_t		blocks_allocated = 0;
	uint32_t		len;
	int			error;

	while (blocks_allocated < nr_blocks)  {
		xfs_fsblock_t	fsbno;

		/*
		 * Grab the smallest extent and use it up, then get the
		 * next smallest.  This mimics the init_*_cursor code.
		 */
		ext_ptr = findfirst_bcnt_extent(agno);
		if (!ext_ptr)
			do_error(
_("error - not enough free space in filesystem\n"));

		/* Use up the extent we've got. */
		len = min(ext_ptr->ex_blockcount, nr_blocks - blocks_allocated);
		fsbno = XFS_AGB_TO_FSB(mp, agno, ext_ptr->ex_startblock);
		error = bulkload_add_blocks(&btr->newbt, fsbno, len);
		if (error)
			do_error(_("could not set up btree reservation: %s\n"),
				strerror(-error));

		error = rmap_add_ag_rec(mp, agno, ext_ptr->ex_startblock, len,
				btr->newbt.oinfo.oi_owner);
		if (error)
			do_error(_("could not set up btree rmaps: %s\n"),
				strerror(-error));

		consume_freespace(agno, ext_ptr, len);
		blocks_allocated += len;
	}
#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "blocks_allocated = %d\n",
		blocks_allocated);
#endif
}

/* Feed one of the new btree blocks to the bulk loader. */
static int
rebuild_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct bt_rebuild	*btr = priv;

	return bulkload_claim_block(cur, &btr->newbt, ptr);
}

/*
 * Scoop up leftovers from a rebuild cursor for later freeing, then free the
 * rebuild context.
 */
void
finish_rebuild(
	struct xfs_mount	*mp,
	struct bt_rebuild	*btr,
	struct xfs_slab		*lost_fsb)
{
	struct bulkload_resv	*resv, *n;

	for_each_bulkload_reservation(&btr->newbt, resv, n) {
		while (resv->used < resv->len) {
			xfs_fsblock_t	fsb = resv->fsbno + resv->used;
			int		error;

			error = slab_add(lost_fsb, &fsb);
			if (error)
				do_error(
_("Insufficient memory saving lost blocks.\n"));
			resv->used++;
		}
	}

	bulkload_destroy(&btr->newbt, 0);
}

/*
 * Free Space Btrees
 *
 * We need to leave some free records in the tree for the corner case of
 * setting up the AGFL. This may require allocation of blocks, and as
 * such can require insertion of new records into the tree (e.g. moving
 * a record in the by-count tree when a long extent is shortened). If we
 * pack the records into the leaves with no slack space, this requires a
 * leaf split to occur and a block to be allocated from the free list.
 * If we don't have any blocks on the free list (because we are setting
 * it up!), then we fail, and the filesystem will fail with the same
 * failure at runtime. Hence leave a couple of records slack space in
 * each block to allow immediate modification of the tree without
 * requiring splits to be done.
 */

/*
 * Return the next free space extent tree record from the previous value we
 * saw.
 */
static inline struct extent_tree_node *
get_bno_rec(
	struct xfs_btree_cur	*cur,
	struct extent_tree_node	*prev_value)
{
	xfs_agnumber_t		agno = cur->bc_ag.agno;

	if (cur->bc_btnum == XFS_BTNUM_BNO) {
		if (!prev_value)
			return findfirst_bno_extent(agno);
		return findnext_bno_extent(prev_value);
	}

	/* cnt btree */
	if (!prev_value)
		return findfirst_bcnt_extent(agno);
	return findnext_bcnt_extent(agno, prev_value);
}

/* Grab one bnobt record and put it in the btree cursor. */
static int
get_bnobt_record(
	struct xfs_btree_cur		*cur,
	void				*priv)
{
	struct bt_rebuild		*btr = priv;
	struct xfs_alloc_rec_incore	*arec = &cur->bc_rec.a;

	btr->bno_rec = get_bno_rec(cur, btr->bno_rec);
	arec->ar_startblock = btr->bno_rec->ex_startblock;
	arec->ar_blockcount = btr->bno_rec->ex_blockcount;
	btr->freeblks += btr->bno_rec->ex_blockcount;
	return 0;
}

void
init_freespace_cursors(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	unsigned int		free_space,
	unsigned int		*nr_extents,
	int			*extra_blocks,
	struct bt_rebuild	*btr_bno,
	struct bt_rebuild	*btr_cnt)
{
	unsigned int		bno_blocks;
	unsigned int		cnt_blocks;
	int			error;

	init_rebuild(sc, &XFS_RMAP_OINFO_AG, free_space, btr_bno);
	init_rebuild(sc, &XFS_RMAP_OINFO_AG, free_space, btr_cnt);

	btr_bno->cur = libxfs_allocbt_stage_cursor(sc->mp,
			&btr_bno->newbt.afake, agno, XFS_BTNUM_BNO);
	btr_cnt->cur = libxfs_allocbt_stage_cursor(sc->mp,
			&btr_cnt->newbt.afake, agno, XFS_BTNUM_CNT);

	btr_bno->bload.get_record = get_bnobt_record;
	btr_bno->bload.claim_block = rebuild_claim_block;

	btr_cnt->bload.get_record = get_bnobt_record;
	btr_cnt->bload.claim_block = rebuild_claim_block;

	/*
	 * Now we need to allocate blocks for the free space btrees using the
	 * free space records we're about to put in them.  Every record we use
	 * can change the shape of the free space trees, so we recompute the
	 * btree shape until we stop needing /more/ blocks.  If we have any
	 * left over we'll stash them in the AGFL when we're done.
	 */
	do {
		unsigned int	num_freeblocks;

		bno_blocks = btr_bno->bload.nr_blocks;
		cnt_blocks = btr_cnt->bload.nr_blocks;

		/* Compute how many bnobt blocks we'll need. */
		error = -libxfs_btree_bload_compute_geometry(btr_bno->cur,
				&btr_bno->bload, *nr_extents);
		if (error)
			do_error(
_("Unable to compute free space by block btree geometry, error %d.\n"), -error);

		/* Compute how many cntbt blocks we'll need. */
		error = -libxfs_btree_bload_compute_geometry(btr_cnt->cur,
				&btr_cnt->bload, *nr_extents);
		if (error)
			do_error(
_("Unable to compute free space by length btree geometry, error %d.\n"), -error);

		/* We don't need any more blocks, so we're done. */
		if (bno_blocks >= btr_bno->bload.nr_blocks &&
		    cnt_blocks >= btr_cnt->bload.nr_blocks)
			break;

		/* Allocate however many more blocks we need this time. */
		if (bno_blocks < btr_bno->bload.nr_blocks)
			reserve_btblocks(sc->mp, agno, btr_bno,
					btr_bno->bload.nr_blocks - bno_blocks);
		if (cnt_blocks < btr_cnt->bload.nr_blocks)
			reserve_btblocks(sc->mp, agno, btr_cnt,
					btr_cnt->bload.nr_blocks - cnt_blocks);

		/* Ok, now how many free space records do we have? */
		*nr_extents = count_bno_extents_blocks(agno, &num_freeblocks);
	} while (1);

	*extra_blocks = (bno_blocks - btr_bno->bload.nr_blocks) +
			(cnt_blocks - btr_cnt->bload.nr_blocks);
}

/* Rebuild the free space btrees. */
void
build_freespace_btrees(
	struct repair_ctx	*sc,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_bno,
	struct bt_rebuild	*btr_cnt)
{
	int			error;

	/* Add all observed bnobt records. */
	error = -libxfs_btree_bload(btr_bno->cur, &btr_bno->bload, btr_bno);
	if (error)
		do_error(
_("Error %d while creating bnobt btree for AG %u.\n"), error, agno);

	/* Add all observed cntbt records. */
	error = -libxfs_btree_bload(btr_cnt->cur, &btr_cnt->bload, btr_cnt);
	if (error)
		do_error(
_("Error %d while creating cntbt btree for AG %u.\n"), error, agno);

	/* Since we're not writing the AGF yet, no need to commit the cursor */
	libxfs_btree_del_cursor(btr_bno->cur, 0);
	libxfs_btree_del_cursor(btr_cnt->cur, 0);
}
