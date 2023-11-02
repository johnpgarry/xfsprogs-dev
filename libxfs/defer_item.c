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
#include "xfs_da_format.h"
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
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "libxfs.h"
#include "defer_item.h"
#include "xfs_ag.h"
#include "xfs_swapext.h"

/* Dummy defer item ops, since we don't do logging. */

/* Extent Freeing */

/* Sort bmap items by AG. */
static int
xfs_extent_free_diff_items(
	void				*priv,
	const struct list_head		*a,
	const struct list_head		*b)
{
	const struct xfs_extent_free_item *ra;
	const struct xfs_extent_free_item *rb;

	ra = container_of(a, struct xfs_extent_free_item, xefi_list);
	rb = container_of(b, struct xfs_extent_free_item, xefi_list);

	return ra->xefi_pag->pag_agno - rb->xefi_pag->pag_agno;
}

/* Get an EFI. */
static struct xfs_log_item *
xfs_extent_free_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_extent_free_diff_items);
	return NULL;
}

/* Get an EFD so we can process all the free extents. */
static struct xfs_log_item *
xfs_extent_free_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Take an active ref to the AG containing the space we're freeing. */
void
xfs_extent_free_get_group(
	struct xfs_mount		*mp,
	struct xfs_extent_free_item	*xefi)
{
	xefi->xefi_pag = xfs_perag_intent_get(mp, xefi->xefi_startblock);
}

/* Release an active AG ref after some freeing work. */
static inline void
xfs_extent_free_put_group(
	struct xfs_extent_free_item	*xefi)
{
	xfs_perag_intent_put(xefi->xefi_pag);
}

/* Process a free extent. */
STATIC int
xfs_extent_free_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_owner_info		oinfo = { };
	struct xfs_extent_free_item	*xefi;
	xfs_agblock_t			agbno;
	int				error = 0;

	xefi = container_of(item, struct xfs_extent_free_item, xefi_list);

	oinfo.oi_owner = xefi->xefi_owner;
	if (xefi->xefi_flags & XFS_EFI_ATTR_FORK)
		oinfo.oi_flags |= XFS_OWNER_INFO_ATTR_FORK;
	if (xefi->xefi_flags & XFS_EFI_BMBT_BLOCK)
		oinfo.oi_flags |= XFS_OWNER_INFO_BMBT_BLOCK;

	agbno = XFS_FSB_TO_AGBNO(tp->t_mountp, xefi->xefi_startblock);

	if (!(xefi->xefi_flags & XFS_EFI_CANCELLED)) {
		error = xfs_free_extent(tp, xefi->xefi_pag, agbno,
				xefi->xefi_blockcount, &oinfo,
				XFS_AG_RESV_NONE);
	}

	/*
	 * Don't free the XEFI if we need a new transaction to complete
	 * processing of it.
	 */
	if (error == -EAGAIN)
		return error;

	xfs_extent_free_put_group(xefi);
	kmem_cache_free(xfs_extfree_item_cache, xefi);
	return error;
}

/* Abort all pending EFIs. */
STATIC void
xfs_extent_free_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a free extent. */
STATIC void
xfs_extent_free_cancel_item(
	struct list_head		*item)
{
	struct xfs_extent_free_item	*xefi;

	xefi = container_of(item, struct xfs_extent_free_item, xefi_list);

	xfs_extent_free_put_group(xefi);
	kmem_cache_free(xfs_extfree_item_cache, xefi);
}

const struct xfs_defer_op_type xfs_extent_free_defer_type = {
	.name		= "extent_free",
	.create_intent	= xfs_extent_free_create_intent,
	.abort_intent	= xfs_extent_free_abort_intent,
	.create_done	= xfs_extent_free_create_done,
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
	struct xfs_owner_info		oinfo = { };
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_extent_free_item	*xefi;
	struct xfs_buf			*agbp;
	int				error;
	xfs_agblock_t			agbno;

	xefi = container_of(item, struct xfs_extent_free_item, xefi_list);

	ASSERT(xefi->xefi_blockcount == 1);
	agbno = XFS_FSB_TO_AGBNO(mp, xefi->xefi_startblock);
	oinfo.oi_owner = xefi->xefi_owner;

	error = xfs_alloc_read_agf(xefi->xefi_pag, tp, 0, &agbp);
	if (!error)
		error = xfs_free_agfl_block(tp, xefi->xefi_pag->pag_agno,
				agbno, agbp, &oinfo);

	xfs_extent_free_put_group(xefi);
	kmem_cache_free(xfs_extfree_item_cache, xefi);
	return error;
}

/* sub-type with special handling for AGFL deferred frees */
const struct xfs_defer_op_type xfs_agfl_free_defer_type = {
	.name		= "agfl_free",
	.create_intent	= xfs_extent_free_create_intent,
	.abort_intent	= xfs_extent_free_abort_intent,
	.create_done	= xfs_extent_free_create_done,
	.finish_item	= xfs_agfl_free_finish_item,
	.cancel_item	= xfs_extent_free_cancel_item,
};

/* Reverse Mapping */

/* Sort rmap intents by AG. */
static int
xfs_rmap_update_diff_items(
	void				*priv,
	const struct list_head		*a,
	const struct list_head		*b)
{
	const struct xfs_rmap_intent	*ra;
	const struct xfs_rmap_intent	*rb;

	ra = container_of(a, struct xfs_rmap_intent, ri_list);
	rb = container_of(b, struct xfs_rmap_intent, ri_list);

	return ra->ri_pag->pag_agno - rb->ri_pag->pag_agno;
}

/* Get an RUI. */
static struct xfs_log_item *
xfs_rmap_update_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	 struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_rmap_update_diff_items);
	return NULL;
}

/* Get an RUD so we can process all the deferred rmap updates. */
static struct xfs_log_item *
xfs_rmap_update_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Take an active ref to the AG containing the space we're rmapping. */
void
xfs_rmap_update_get_group(
	struct xfs_mount	*mp,
	struct xfs_rmap_intent	*ri)
{
	ri->ri_pag = xfs_perag_intent_get(mp, ri->ri_bmap.br_startblock);
}

/* Release an active AG ref after finishing rmapping work. */
static inline void
xfs_rmap_update_put_group(
	struct xfs_rmap_intent	*ri)
{
	xfs_perag_intent_put(ri->ri_pag);
}

/* Process a deferred rmap update. */
STATIC int
xfs_rmap_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_rmap_intent		*ri;
	int				error;

	ri = container_of(item, struct xfs_rmap_intent, ri_list);

	error = xfs_rmap_finish_one(tp, ri, state);

	xfs_rmap_update_put_group(ri);
	kmem_cache_free(xfs_rmap_intent_cache, ri);
	return error;
}

/* Abort all pending RUIs. */
STATIC void
xfs_rmap_update_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a deferred rmap update. */
STATIC void
xfs_rmap_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_rmap_intent		*ri;

	ri = container_of(item, struct xfs_rmap_intent, ri_list);

	xfs_rmap_update_put_group(ri);
	kmem_cache_free(xfs_rmap_intent_cache, ri);
}

const struct xfs_defer_op_type xfs_rmap_update_defer_type = {
	.name		= "rmap",
	.create_intent	= xfs_rmap_update_create_intent,
	.abort_intent	= xfs_rmap_update_abort_intent,
	.create_done	= xfs_rmap_update_create_done,
	.finish_item	= xfs_rmap_update_finish_item,
	.finish_cleanup = xfs_rmap_finish_one_cleanup,
	.cancel_item	= xfs_rmap_update_cancel_item,
};

/* Reference Counting */

/* Sort refcount intents by AG. */
static int
xfs_refcount_update_diff_items(
	void				*priv,
	const struct list_head		*a,
	const struct list_head		*b)
{
	const struct xfs_refcount_intent *ra;
	const struct xfs_refcount_intent *rb;

	ra = container_of(a, struct xfs_refcount_intent, ri_list);
	rb = container_of(b, struct xfs_refcount_intent, ri_list);

	return ra->ri_pag->pag_agno - rb->ri_pag->pag_agno;
}

/* Get an CUI. */
static struct xfs_log_item *
xfs_refcount_update_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_refcount_update_diff_items);
	return NULL;
}

/* Get an CUD so we can process all the deferred refcount updates. */
static struct xfs_log_item *
xfs_refcount_update_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Take an active ref to the AG containing the space we're refcounting. */
void
xfs_refcount_update_get_group(
	struct xfs_mount		*mp,
	struct xfs_refcount_intent	*ri)
{
	ri->ri_pag = xfs_perag_intent_get(mp, ri->ri_startblock);
}

/* Release an active AG ref after finishing refcounting work. */
static inline void
xfs_refcount_update_put_group(
	struct xfs_refcount_intent	*ri)
{
	xfs_perag_intent_put(ri->ri_pag);
}

/* Process a deferred refcount update. */
STATIC int
xfs_refcount_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_refcount_intent	*ri;
	int				error;

	ri = container_of(item, struct xfs_refcount_intent, ri_list);
	error = xfs_refcount_finish_one(tp, ri, state);

	/* Did we run out of reservation?  Requeue what we didn't finish. */
	if (!error && ri->ri_blockcount > 0) {
		ASSERT(ri->ri_type == XFS_REFCOUNT_INCREASE ||
		       ri->ri_type == XFS_REFCOUNT_DECREASE);
		return -EAGAIN;
	}

	xfs_refcount_update_put_group(ri);
	kmem_cache_free(xfs_refcount_intent_cache, ri);
	return error;
}

/* Abort all pending CUIs. */
STATIC void
xfs_refcount_update_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a deferred refcount update. */
STATIC void
xfs_refcount_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_refcount_intent	*ri;

	ri = container_of(item, struct xfs_refcount_intent, ri_list);

	xfs_refcount_update_put_group(ri);
	kmem_cache_free(xfs_refcount_intent_cache, ri);
}

const struct xfs_defer_op_type xfs_refcount_update_defer_type = {
	.name		= "refcount",
	.create_intent	= xfs_refcount_update_create_intent,
	.abort_intent	= xfs_refcount_update_abort_intent,
	.create_done	= xfs_refcount_update_create_done,
	.finish_item	= xfs_refcount_update_finish_item,
	.finish_cleanup = xfs_refcount_finish_one_cleanup,
	.cancel_item	= xfs_refcount_update_cancel_item,
};

/* Inode Block Mapping */

static inline struct xfs_bmap_intent *bi_entry(const struct list_head *e)
{
	return list_entry(e, struct xfs_bmap_intent, bi_list);
}

/* Sort bmap intents by inode. */
static int
xfs_bmap_update_diff_items(
	void				*priv,
	const struct list_head		*a,
	const struct list_head		*b)
{
	struct xfs_bmap_intent		*ba = bi_entry(a);
	struct xfs_bmap_intent		*bb = bi_entry(b);

	return ba->bi_owner->i_ino - bb->bi_owner->i_ino;
}

/* Get an BUI. */
static struct xfs_log_item *
xfs_bmap_update_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_bmap_update_diff_items);
	return NULL;
}

/* Get an BUD so we can process all the deferred rmap updates. */
static struct xfs_log_item *
xfs_bmap_update_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Take an active ref to the AG containing the space we're mapping. */
static inline void
xfs_bmap_update_get_group(
	struct xfs_mount	*mp,
	struct xfs_bmap_intent	*bi)
{
	if (xfs_ifork_is_realtime(bi->bi_owner, bi->bi_whichfork)) {
		if (xfs_has_rtgroups(mp)) {
			xfs_rgnumber_t	rgno;

			rgno = xfs_rtb_to_rgno(mp, bi->bi_bmap.br_startblock);
			bi->bi_rtg = xfs_rtgroup_get(mp, rgno);
		} else {
			bi->bi_rtg = NULL;
		}

		return;
	}

	/*
	 * Bump the intent count on behalf of the deferred rmap and refcount
	 * intent items that that we can queue when we finish this bmap work.
	 * This new intent item will bump the intent count before the bmap
	 * intent drops the intent count, ensuring that the intent count
	 * remains nonzero across the transaction roll.
	 */
	bi->bi_pag = xfs_perag_intent_get(mp, bi->bi_bmap.br_startblock);
}

/* Add this deferred BUI to the transaction. */
void
xfs_bmap_defer_add(
	struct xfs_trans	*tp,
	struct xfs_bmap_intent	*bi)
{
	trace_xfs_bmap_defer(bi);

	xfs_bmap_update_get_group(tp->t_mountp, bi);
	xfs_defer_add(tp, &bi->bi_list, &xfs_bmap_update_defer_type);
}

/* Release an active AG ref after finishing mapping work. */
static inline void
xfs_bmap_update_put_group(
	struct xfs_bmap_intent	*bi)
{
	if (xfs_ifork_is_realtime(bi->bi_owner, bi->bi_whichfork)) {
		if (xfs_has_rtgroups(bi->bi_owner->i_mount))
			xfs_rtgroup_put(bi->bi_rtg);
		return;
	}

	xfs_perag_intent_put(bi->bi_pag);
}

/* Cancel a deferred rmap update. */
STATIC void
xfs_bmap_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_bmap_intent		*bi = bi_entry(item);

	xfs_bmap_update_put_group(bi);
	kmem_cache_free(xfs_bmap_intent_cache, bi);
}

/* Process a deferred rmap update. */
STATIC int
xfs_bmap_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_bmap_intent		*bi = bi_entry(item);
	int				error;

	error = xfs_bmap_finish_one(tp, bi);
	if (!error && bi->bi_bmap.br_blockcount > 0) {
		ASSERT(bi->bi_type == XFS_BMAP_UNMAP);
		return -EAGAIN;
	}

	xfs_bmap_update_cancel_item(item);
	return error;
}

/* Abort all pending BUIs. */
STATIC void
xfs_bmap_update_abort_intent(
	struct xfs_log_item		*intent)
{
}

const struct xfs_defer_op_type xfs_bmap_update_defer_type = {
	.name		= "bmap",
	.create_intent	= xfs_bmap_update_create_intent,
	.abort_intent	= xfs_bmap_update_abort_intent,
	.create_done	= xfs_bmap_update_create_done,
	.finish_item	= xfs_bmap_update_finish_item,
	.cancel_item	= xfs_bmap_update_cancel_item,
};

/* Logged extended attributes */

static inline struct xfs_attr_intent *attri_entry(const struct list_head *e)
{
	return list_entry(e, struct xfs_attr_intent, xattri_list);
}

/* Get an ATTRI. */
static struct xfs_log_item *
xfs_attr_create_intent(
	struct xfs_trans	*tp,
	struct list_head	*items,
	unsigned int		count,
	bool			sort)
{
	return NULL;
}

/* Abort all pending ATTRs. */
static void
xfs_attr_abort_intent(
	struct xfs_log_item	*intent)
{
}

/* Get an ATTRD so we can process all the attrs. */
static struct xfs_log_item *
xfs_attr_create_done(
	struct xfs_trans	*tp,
	struct xfs_log_item	*intent,
	unsigned int		count)
{
	return NULL;
}

static inline void
xfs_attr_free_item(
	struct xfs_attr_intent	*attr)
{
	if (attr->xattri_da_state)
		xfs_da_state_free(attr->xattri_da_state);
	if (attr->xattri_da_args->op_flags & XFS_DA_OP_RECOVERY)
		kmem_free(attr);
	else
		kmem_cache_free(xfs_attr_intent_cache, attr);
}

/* Process an attr. */
static int
xfs_attr_finish_item(
	struct xfs_trans	*tp,
	struct xfs_log_item	*done,
	struct list_head	*item,
	struct xfs_btree_cur	**state)
{
	struct xfs_attr_intent	*attr = attri_entry(item);
	struct xfs_da_args	*args;
	int			error;

	args = attr->xattri_da_args;

	/*
	 * Always reset trans after EAGAIN cycle
	 * since the transaction is new
	 */
	args->trans = tp;

	if (XFS_TEST_ERROR(false, args->dp->i_mount, XFS_ERRTAG_LARP)) {
		error = -EIO;
		goto out;
	}

	error = xfs_attr_set_iter(attr);
	if (!error && attr->xattri_dela_state != XFS_DAS_DONE)
		error = -EAGAIN;
out:
	if (error != -EAGAIN)
		xfs_attr_free_item(attr);

	return error;
}

/* Cancel an attr */
static void
xfs_attr_cancel_item(
	struct list_head	*item)
{
	struct xfs_attr_intent	*attr = attri_entry(item);

	xfs_attr_free_item(attr);
}

const struct xfs_defer_op_type xfs_attr_defer_type = {
	.name		= "attr",
	.max_items	= 1,
	.create_intent	= xfs_attr_create_intent,
	.abort_intent	= xfs_attr_abort_intent,
	.create_done	= xfs_attr_create_done,
	.finish_item	= xfs_attr_finish_item,
	.cancel_item	= xfs_attr_cancel_item,
};

/* Atomic Swapping of File Ranges */

STATIC struct xfs_log_item *
xfs_swapext_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	return NULL;
}
STATIC struct xfs_log_item *
xfs_swapext_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Add this deferred SXI to the transaction. */
void
xfs_swapext_defer_add(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	trace_xfs_swapext_defer(tp->t_mountp, sxi);

	xfs_defer_add(tp, &sxi->sxi_list, &xfs_swapext_defer_type);
}

static inline struct xfs_swapext_intent *sxi_entry(const struct list_head *e)
{
	return list_entry(e, struct xfs_swapext_intent, sxi_list);
}

/* Process a deferred swapext update. */
STATIC int
xfs_swapext_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_swapext_intent	*sxi = sxi_entry(item);
	int				error;

	/*
	 * Swap one more extent between the two files.  If there's still more
	 * work to do, we want to requeue ourselves after all other pending
	 * deferred operations have finished.  This includes all of the dfops
	 * that we queued directly as well as any new ones created in the
	 * process of finishing the others.  Doing so prevents us from queuing
	 * a large number of SXI log items in kernel memory, which in turn
	 * prevents us from pinning the tail of the log (while logging those
	 * new SXI items) until the first SXI items can be processed.
	 */
	error = xfs_swapext_finish_one(tp, sxi);
	if (error != -EAGAIN)
		kmem_cache_free(xfs_swapext_intent_cache, sxi);
	return error;
}

/* Abort all pending SXIs. */
STATIC void
xfs_swapext_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a deferred swapext update. */
STATIC void
xfs_swapext_cancel_item(
	struct list_head		*item)
{
	struct xfs_swapext_intent	*sxi = sxi_entry(item);

	kmem_cache_free(xfs_swapext_intent_cache, sxi);
}

const struct xfs_defer_op_type xfs_swapext_defer_type = {
	.name		= "swapext",
	.create_intent	= xfs_swapext_create_intent,
	.abort_intent	= xfs_swapext_abort_intent,
	.create_done	= xfs_swapext_create_done,
	.finish_item	= xfs_swapext_finish_item,
	.cancel_item	= xfs_swapext_cancel_item,
};
