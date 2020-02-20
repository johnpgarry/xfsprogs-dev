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
