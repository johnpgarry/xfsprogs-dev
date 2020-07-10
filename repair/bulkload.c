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
int
bulkload_add_blocks(
	struct bulkload		*bkl,
	xfs_fsblock_t		fsbno,
	xfs_extlen_t		len)
{
	struct bulkload_resv	*resv;

	resv = kmem_alloc(sizeof(struct bulkload_resv), KM_MAYFAIL);
	if (!resv)
		return ENOMEM;

	INIT_LIST_HEAD(&resv->list);
	resv->fsbno = fsbno;
	resv->len = len;
	resv->used = 0;
	list_add_tail(&resv->list, &bkl->resv_list);
	return 0;
}

/* Free all the accounting info and disk space we reserved for a new btree. */
void
bulkload_destroy(
	struct bulkload		*bkl,
	int			error)
{
	struct bulkload_resv	*resv, *n;

	list_for_each_entry_safe(resv, n, &bkl->resv_list, list) {
		list_del(&resv->list);
		kmem_free(resv);
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
	xfs_fsblock_t		fsb;

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
	fsb = resv->fsbno + resv->used;
	resv->used++;

	/* If we used all the blocks in this reservation, move it to the end. */
	if (resv->used == resv->len)
		list_move_tail(&resv->list, &bkl->resv_list);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		ptr->l = cpu_to_be64(fsb);
	else
		ptr->s = cpu_to_be32(XFS_FSB_TO_AGBNO(cur->bc_mp, fsb));
	return 0;
}
