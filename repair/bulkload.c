// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <libxfs.h>
#include "bulkload.h"

int bload_leaf_slack = -1;
int bload_node_slack = -1;

/* Initialize accounting resources for staging a new AG btree. */
void
bulkload_init_ag(
	struct bulkload			*bkl,
	struct repair_ctx		*sc,
	const struct xfs_owner_info	*oinfo)
{
	memset(bkl, 0, sizeof(struct bulkload));
	bkl->sc = sc;
	bkl->oinfo = *oinfo; /* structure copy */
	INIT_LIST_HEAD(&bkl->resv_list);
}

/* Designate specific blocks to be used to build our new btree. */
static int
bulkload_add_blocks(
	struct bulkload			*bkl,
	struct xfs_perag		*pag,
	const struct xfs_alloc_arg	*args)
{
	struct xfs_mount		*mp = bkl->sc->mp;
	struct bulkload_resv		*resv;

	resv = kmalloc(sizeof(struct bulkload_resv), GFP_KERNEL);
	if (!resv)
		return ENOMEM;

	INIT_LIST_HEAD(&resv->list);
	resv->agbno = XFS_FSB_TO_AGBNO(mp, args->fsbno);
	resv->len = args->len;
	resv->used = 0;
	resv->pag = libxfs_perag_hold(pag);

	list_add_tail(&resv->list, &bkl->resv_list);
	bkl->nr_reserved += args->len;
	return 0;
}

/*
 * Add an extent to the new btree reservation pool.  Callers are required to
 * reap this reservation manually if the repair is cancelled.  @pag must be a
 * passive reference.
 */
int
bulkload_add_extent(
	struct bulkload		*bkl,
	struct xfs_perag	*pag,
	xfs_agblock_t		agbno,
	xfs_extlen_t		len)
{
	struct xfs_mount	*mp = bkl->sc->mp;
	struct xfs_alloc_arg	args = {
		.tp		= NULL, /* no autoreap */
		.oinfo		= bkl->oinfo,
		.fsbno		= XFS_AGB_TO_FSB(mp, pag->pag_agno, agbno),
		.len		= len,
		.resv		= XFS_AG_RESV_NONE,
	};

	return bulkload_add_blocks(bkl, pag, &args);
}

/* Free all the accounting info and disk space we reserved for a new btree. */
void
bulkload_commit(
	struct bulkload		*bkl)
{
	struct bulkload_resv	*resv, *n;

	list_for_each_entry_safe(resv, n, &bkl->resv_list, list) {
		list_del(&resv->list);
		kfree(resv);
	}
}

/* Feed one of the reserved btree blocks to the bulk loader. */
int
bulkload_claim_block(
	struct xfs_btree_cur	*cur,
	struct bulkload		*bkl,
	union xfs_btree_ptr	*ptr)
{
	struct bulkload_resv	*resv;
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_agblock_t		agbno;

	/*
	 * The first item in the list should always have a free block unless
	 * we're completely out.
	 */
	resv = list_first_entry(&bkl->resv_list, struct bulkload_resv, list);
	if (resv->used == resv->len)
		return ENOSPC;

	/*
	 * Peel off a block from the start of the reservation.  We allocate
	 * blocks in order to place blocks on disk in increasing record or key
	 * order.  The block reservations tend to end up on the list in
	 * decreasing order, which hopefully results in leaf blocks ending up
	 * together.
	 */
	agbno = resv->agbno + resv->used;
	resv->used++;

	/* If we used all the blocks in this reservation, move it to the end. */
	if (resv->used == resv->len)
		list_move_tail(&resv->list, &bkl->resv_list);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		ptr->l = cpu_to_be64(XFS_AGB_TO_FSB(mp, resv->pag->pag_agno,
								agbno));
	else
		ptr->s = cpu_to_be32(agbno);
	return 0;
}

/*
 * Estimate proper slack values for a btree that's being reloaded.
 *
 * Under most circumstances, we'll take whatever default loading value the
 * btree bulk loading code calculates for us.  However, there are some
 * exceptions to this rule:
 *
 * (1) If someone turned one of the debug knobs.
 * (2) The AG has less than ~10% space free.
 *
 * In the latter case, format the new btree blocks almost completely full to
 * minimize space usage.
 */
void
bulkload_estimate_ag_slack(
	struct repair_ctx	*sc,
	struct xfs_btree_bload	*bload,
	unsigned int		free)
{
	/*
	 * The global values are set to -1 (i.e. take the bload defaults)
	 * unless someone has set them otherwise, so we just pull the values
	 * here.
	 */
	bload->leaf_slack = bload_leaf_slack;
	bload->node_slack = bload_node_slack;

	/* No further changes if there's more than 10% space left. */
	if (free >= sc->mp->m_sb.sb_agblocks / 10)
		return;

	/*
	 * We're low on space; load the btrees as tightly as possible.  Leave
	 * a couple of open slots in each btree block so that we don't end up
	 * splitting the btrees like crazy right after mount.
	 */
	if (bload->leaf_slack < 0)
		bload->leaf_slack = 2;
	if (bload->node_slack < 0)
		bload->node_slack = 2;
}
