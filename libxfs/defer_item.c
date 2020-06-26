// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_alloc.h"
#include "xfs_rmap.h"
#include "xfs_refcount.h"
#include "xfs_bmap.h"
#include "xfs_inode.h"

/* Dummy defer item ops, since we don't do logging. */

static struct xfs_log_item *
noop_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	return NULL;
}

static struct xfs_log_item *
noop_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

static void
noop_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Extent Freeing */

/* Process a free extent. */
STATIC int
xfs_extent_free_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_extent_free_item	*free;
	int				error;

	free = container_of(item, struct xfs_extent_free_item, xefi_list);
	error = xfs_free_extent(tp, free->xefi_startblock,
			free->xefi_blockcount, &free->xefi_oinfo,
			XFS_AG_RESV_NONE);
	kmem_free(free);
	return error;
}

/* Cancel a free extent. */
STATIC void
xfs_extent_free_cancel_item(
	struct list_head		*item)
{
	struct xfs_extent_free_item	*free;

	free = container_of(item, struct xfs_extent_free_item, xefi_list);
	kmem_free(free);
}

const struct xfs_defer_op_type xfs_extent_free_defer_type = {
	.create_intent	= noop_create_intent,
	.abort_intent	= noop_abort_intent,
	.create_done	= noop_create_done,
	.finish_item	= xfs_extent_free_finish_item,
	.cancel_item	= xfs_extent_free_cancel_item,
};

/*
 * AGFL blocks are accounted differently in the reserve pools and are not
 * inserted into the busy extent list.
 */
STATIC int
xfs_agfl_free_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_extent_free_item	*free;
	struct xfs_buf			*agbp;
	int				error;
	xfs_agnumber_t			agno;
	xfs_agblock_t			agbno;

	free = container_of(item, struct xfs_extent_free_item, xefi_list);
	ASSERT(free->xefi_blockcount == 1);
	agno = XFS_FSB_TO_AGNO(mp, free->xefi_startblock);
	agbno = XFS_FSB_TO_AGBNO(mp, free->xefi_startblock);

	error = xfs_alloc_read_agf(mp, tp, agno, 0, &agbp);
	if (!error)
		error = xfs_free_agfl_block(tp, agno, agbno, agbp,
					    &free->xefi_oinfo);
	kmem_free(free);
	return error;
}

/* sub-type with special handling for AGFL deferred frees */
const struct xfs_defer_op_type xfs_agfl_free_defer_type = {
	.create_intent	= noop_create_intent,
	.abort_intent	= noop_abort_intent,
	.create_done	= noop_create_done,
	.finish_item	= xfs_agfl_free_finish_item,
	.cancel_item	= xfs_extent_free_cancel_item,
};

/* Reverse Mapping */

/* Process a deferred rmap update. */
STATIC int
xfs_rmap_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_rmap_intent		*rmap;
	int				error;

	rmap = container_of(item, struct xfs_rmap_intent, ri_list);
	error = xfs_rmap_finish_one(tp,
			rmap->ri_type,
			rmap->ri_owner, rmap->ri_whichfork,
			rmap->ri_bmap.br_startoff,
			rmap->ri_bmap.br_startblock,
			rmap->ri_bmap.br_blockcount,
			rmap->ri_bmap.br_state,
			state);
	kmem_free(rmap);
	return error;
}

/* Clean up after processing deferred rmaps. */
STATIC void
xfs_rmap_update_finish_cleanup(
	struct xfs_trans	*tp,
	struct xfs_btree_cur	*rcur,
	int			error)
{
	xfs_rmap_finish_one_cleanup(tp, rcur, error);
}

/* Cancel a deferred rmap update. */
STATIC void
xfs_rmap_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_rmap_intent		*rmap;

	rmap = container_of(item, struct xfs_rmap_intent, ri_list);
	kmem_free(rmap);
}

const struct xfs_defer_op_type xfs_rmap_update_defer_type = {
	.create_intent	= noop_create_intent,
	.abort_intent	= noop_abort_intent,
	.create_done	= noop_create_done,
	.finish_item	= xfs_rmap_update_finish_item,
	.finish_cleanup = xfs_rmap_update_finish_cleanup,
	.cancel_item	= xfs_rmap_update_cancel_item,
};

/* Reference Counting */

/* Process a deferred refcount update. */
STATIC int
xfs_refcount_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_refcount_intent	*refc;
	xfs_fsblock_t			new_fsb;
	xfs_extlen_t			new_aglen;
	int				error;

	refc = container_of(item, struct xfs_refcount_intent, ri_list);
	error = xfs_refcount_finish_one(tp,
			refc->ri_type,
			refc->ri_startblock,
			refc->ri_blockcount,
			&new_fsb, &new_aglen,
			state);
	/* Did we run out of reservation?  Requeue what we didn't finish. */
	if (!error && new_aglen > 0) {
		ASSERT(refc->ri_type == XFS_REFCOUNT_INCREASE ||
		       refc->ri_type == XFS_REFCOUNT_DECREASE);
		refc->ri_startblock = new_fsb;
		refc->ri_blockcount = new_aglen;
		return -EAGAIN;
	}
	kmem_free(refc);
	return error;
}

/* Clean up after processing deferred refcounts. */
STATIC void
xfs_refcount_update_finish_cleanup(
	struct xfs_trans	*tp,
	struct xfs_btree_cur	*rcur,
	int			error)
{
	xfs_refcount_finish_one_cleanup(tp, rcur, error);
}

/* Cancel a deferred refcount update. */
STATIC void
xfs_refcount_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_refcount_intent	*refc;

	refc = container_of(item, struct xfs_refcount_intent, ri_list);
	kmem_free(refc);
}

const struct xfs_defer_op_type xfs_refcount_update_defer_type = {
	.create_intent	= noop_create_intent,
	.abort_intent	= noop_abort_intent,
	.create_done	= noop_create_done,
	.finish_item	= xfs_refcount_update_finish_item,
	.finish_cleanup = xfs_refcount_update_finish_cleanup,
	.cancel_item	= xfs_refcount_update_cancel_item,
};

/* Inode Block Mapping */

/* Process a deferred bmap update. */
STATIC int
xfs_bmap_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_bmap_intent		*bmap;
	xfs_filblks_t			count;
	int				error;

	bmap = container_of(item, struct xfs_bmap_intent, bi_list);
	count = bmap->bi_bmap.br_blockcount;
	error = xfs_bmap_finish_one(tp,
			bmap->bi_owner,
			bmap->bi_type, bmap->bi_whichfork,
			bmap->bi_bmap.br_startoff,
			bmap->bi_bmap.br_startblock,
			&count,
			bmap->bi_bmap.br_state);
	if (!error && count > 0) {
		ASSERT(bmap->bi_type == XFS_BMAP_UNMAP);
		bmap->bi_bmap.br_blockcount = count;
		return -EAGAIN;
	}
	kmem_free(bmap);
	return error;
}

/* Cancel a deferred bmap update. */
STATIC void
xfs_bmap_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_bmap_intent		*bmap;

	bmap = container_of(item, struct xfs_bmap_intent, bi_list);
	kmem_free(bmap);
}

const struct xfs_defer_op_type xfs_bmap_update_defer_type = {
	.create_intent	= noop_create_intent,
	.abort_intent	= noop_abort_intent,
	.create_done	= noop_create_done,
	.finish_item	= xfs_bmap_update_finish_item,
	.cancel_item	= xfs_bmap_update_cancel_item,
};
