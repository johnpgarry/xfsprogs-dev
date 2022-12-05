/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs_priv.h"
#include "libxfs.h"
#include "xfile.h"
#include "xfbtree.h"
#include "xfs_btree_mem.h"
#include "libfrog/bitmap.h"

/* Extract the buftarg target for this xfile btree. */
struct xfs_buftarg *
xfbtree_target(struct xfbtree *xfbtree)
{
	return xfbtree->target;
}

/* Is this daddr (sector offset) contained within the buffer target? */
static inline bool
xfbtree_verify_buftarg_xfileoff(
	struct xfs_buftarg	*btp,
	xfileoff_t		xfoff)
{
	xfs_daddr_t		xfoff_daddr = xfo_to_daddr(xfoff);

	return xfs_buftarg_verify_daddr(btp, xfoff_daddr);
}

/* Is this btree xfile offset contained within the xfile? */
bool
xfbtree_verify_xfileoff(
	struct xfs_btree_cur	*cur,
	unsigned long long	xfoff)
{
	struct xfs_buftarg	*btp = xfbtree_target(cur->bc_mem.xfbtree);

	return xfbtree_verify_buftarg_xfileoff(btp, xfoff);
}

/* Check if a btree pointer is reasonable. */
int
xfbtree_check_ptr(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				index,
	int				level)
{
	xfileoff_t			bt_xfoff;

	ASSERT(xfs_btree_has_xfile(cur));

	if (xfs_btree_has_long_ptrs(cur))
		bt_xfoff = be64_to_cpu(ptr->l);
	else
		bt_xfoff = be32_to_cpu(ptr->s);

	if (!xfbtree_verify_xfileoff(cur, bt_xfoff)) {
		xfs_err(cur->bc_mp,
"In-memory: Corrupt btree %d flags 0x%x pointer at level %d index %d fa %pS.",
				cur->bc_btnum, cur->bc_flags, level, index,
				__this_address);
		return -EFSCORRUPTED;
	}
	return 0;
}

/* Convert a btree pointer to a daddr */
xfs_daddr_t
xfbtree_ptr_to_daddr(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr)
{
	xfileoff_t			bt_xfoff;

	if (xfs_btree_has_long_ptrs(cur))
		bt_xfoff = be64_to_cpu(ptr->l);
	else
		bt_xfoff = be32_to_cpu(ptr->s);
	return xfo_to_daddr(bt_xfoff);
}

/* Set the pointer to point to this buffer. */
void
xfbtree_buf_to_ptr(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	union xfs_btree_ptr	*ptr)
{
	xfileoff_t		xfoff = xfs_daddr_to_xfo(xfs_buf_daddr(bp));

	if (xfs_btree_has_long_ptrs(cur))
		ptr->l = cpu_to_be64(xfoff);
	else
		ptr->s = cpu_to_be32(xfoff);
}

/* Return the in-memory btree block size, in units of 512 bytes. */
unsigned int xfbtree_bbsize(void)
{
	return xfo_to_daddr(1);
}

/* Set the root of an in-memory btree. */
void
xfbtree_set_root(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				inc)
{
	ASSERT(xfs_btree_has_xfile(cur));

	cur->bc_mem.xfbtree->root = *ptr;
	cur->bc_mem.xfbtree->nlevels += inc;
}

/* Initialize a pointer from the in-memory btree header. */
void
xfbtree_init_ptr_from_cur(
	struct xfs_btree_cur		*cur,
	union xfs_btree_ptr		*ptr)
{
	ASSERT(xfs_btree_has_xfile(cur));

	*ptr = cur->bc_mem.xfbtree->root;
}

/* Duplicate an in-memory btree cursor. */
struct xfs_btree_cur *
xfbtree_dup_cursor(
	struct xfs_btree_cur		*cur)
{
	struct xfs_btree_cur		*ncur;

	ASSERT(xfs_btree_has_xfile(cur));

	ncur = xfs_btree_alloc_cursor(cur->bc_mp, cur->bc_tp, cur->bc_btnum,
			cur->bc_ops, cur->bc_maxlevels, cur->bc_cache);
	ncur->bc_flags = cur->bc_flags;
	ncur->bc_nlevels = cur->bc_nlevels;
	ncur->bc_statoff = cur->bc_statoff;
	memcpy(&ncur->bc_mem, &cur->bc_mem, sizeof(cur->bc_mem));

	if (cur->bc_mem.pag)
		ncur->bc_mem.pag = xfs_perag_hold(cur->bc_mem.pag);

	return ncur;
}

/* Check the owner of an in-memory btree block. */
xfs_failaddr_t
xfbtree_check_block_owner(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;

	if (xfs_btree_has_long_ptrs(cur)) {
		if (be64_to_cpu(block->bb_u.l.bb_owner) != xfbt->owner)
			return __this_address;

		return NULL;
	}

	if (be32_to_cpu(block->bb_u.s.bb_owner) != xfbt->owner)
		return __this_address;

	return NULL;
}

/* Return the owner of this in-memory btree. */
unsigned long long
xfbtree_owner(
	struct xfs_btree_cur	*cur)
{
	return cur->bc_mem.xfbtree->owner;
}

/* Return the xfile offset (in blocks) of a btree buffer. */
unsigned long long
xfbtree_buf_to_xfoff(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp)
{
	ASSERT(xfs_btree_has_xfile(cur));

	return xfs_daddr_to_xfo(xfs_buf_daddr(bp));
}

/* Verify a long-format btree block. */
xfs_failaddr_t
xfbtree_lblock_verify(
	struct xfs_buf		*bp,
	unsigned int		max_recs)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_buftarg	*btp = bp->b_target;

	/* numrecs verification */
	if (be16_to_cpu(block->bb_numrecs) > max_recs)
		return __this_address;

	/* sibling pointer verification */
	if (block->bb_u.l.bb_leftsib != cpu_to_be64(NULLFSBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be64_to_cpu(block->bb_u.l.bb_leftsib)))
		return __this_address;

	if (block->bb_u.l.bb_rightsib != cpu_to_be64(NULLFSBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be64_to_cpu(block->bb_u.l.bb_rightsib)))
		return __this_address;

	return NULL;
}

/* Verify a short-format btree block. */
xfs_failaddr_t
xfbtree_sblock_verify(
	struct xfs_buf		*bp,
	unsigned int		max_recs)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_buftarg	*btp = bp->b_target;

	/* numrecs verification */
	if (be16_to_cpu(block->bb_numrecs) > max_recs)
		return __this_address;

	/* sibling pointer verification */
	if (block->bb_u.s.bb_leftsib != cpu_to_be32(NULLAGBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be32_to_cpu(block->bb_u.s.bb_leftsib)))
		return __this_address;

	if (block->bb_u.s.bb_rightsib != cpu_to_be32(NULLAGBLOCK) &&
	    !xfbtree_verify_buftarg_xfileoff(btp,
				be32_to_cpu(block->bb_u.s.bb_rightsib)))
		return __this_address;

	return NULL;
}

/* Close the btree xfile and release all resources. */
void
xfbtree_destroy(
	struct xfbtree		*xfbt)
{
	bitmap_free(&xfbt->freespace);
	libxfs_buftarg_drain(xfbt->target);
}

/* Compute the number of bytes available for records. */
static inline unsigned int
xfbtree_rec_bytes(
	struct xfs_mount		*mp,
	const struct xfs_btree_ops	*ops)
{
	unsigned int			blocklen = xfo_to_b(1);

	if (ops->geom_flags & XFS_BTGEO_LONG_PTRS) {
		if (ops->geom_flags & XFS_BTGEO_CRC_BLOCKS)
			return blocklen - XFS_BTREE_LBLOCK_CRC_LEN;

		return blocklen - XFS_BTREE_LBLOCK_LEN;
	}

	if (ops->geom_flags & XFS_BTGEO_CRC_BLOCKS)
		return blocklen - XFS_BTREE_SBLOCK_CRC_LEN;

	return blocklen - XFS_BTREE_SBLOCK_LEN;
}

/* Initialize an empty leaf block as the btree root. */
STATIC int
xfbtree_init_leaf_block(
	struct xfs_mount		*mp,
	struct xfbtree			*xfbt,
	const struct xfs_btree_ops	*ops)
{
	struct xfs_buf			*bp;
	xfileoff_t			xfoff = xfbt->highest_offset++;
	int				error;

	error = xfs_buf_get(xfbt->target, xfo_to_daddr(xfoff),
			xfbtree_bbsize(), &bp);
	if (error)
		return error;

	trace_xfbtree_create_root_buf(xfbt, bp);

	bp->b_ops = ops->buf_ops;
	xfs_btree_init_buf(mp, bp, ops, 0, 0, xfbt->owner);
	error = xfs_bwrite(bp);
	xfs_buf_relse(bp);
	if (error)
		return error;

	if (ops->geom_flags & XFS_BTGEO_LONG_PTRS)
		xfbt->root.l = xfoff;
	else
		xfbt->root.s = xfoff;
	return 0;
}

/*
 * Create an xfile btree backing thing that can be used for in-memory btrees.
 * Callers must set xfbt->target and xfbt->owner.
 */
int
xfbtree_init(
	struct xfs_mount		*mp,
	struct xfbtree			*xfbt,
	const struct xfs_btree_ops	*ops)
{
	unsigned int			blocklen = xfbtree_rec_bytes(mp, ops);
	unsigned int			keyptr_len = ops->key_len;
	int				error;

	/* Requires an xfile-backed buftarg. */
	if (!xfbt->target) {
		ASSERT(xfbt->target);
		return -EINVAL;
	}
	if (!(xfbt->target->flags & XFS_BUFTARG_XFILE)) {
		ASSERT(xfbt->target->flags & XFS_BUFTARG_XFILE);
		return -EINVAL;
	}

	error = bitmap_alloc(&xfbt->freespace);
	if (error)
		goto err_buftarg;

	/* Set up min/maxrecs for this btree. */
	if (ops->geom_flags & XFS_BTGEO_LONG_PTRS)
		keyptr_len += sizeof(__be64);
	else
		keyptr_len += sizeof(__be32);
	xfbt->maxrecs[0] = blocklen / ops->rec_len;
	xfbt->maxrecs[1] = blocklen / keyptr_len;
	xfbt->minrecs[0] = xfbt->maxrecs[0] / 2;
	xfbt->minrecs[1] = xfbt->maxrecs[1] / 2;
	xfbt->highest_offset = 0;
	xfbt->nlevels = 1;

	/* Initialize the empty btree. */
	error = xfbtree_init_leaf_block(mp, xfbt, ops);
	if (error)
		goto err_freesp;

	trace_xfbtree_init(mp, xfbt, ops);

	return 0;

err_freesp:
	bitmap_free(&xfbt->freespace);
err_buftarg:
	libxfs_buftarg_drain(xfbt->target);
	return error;
}

static inline struct xfile *xfbtree_xfile(struct xfbtree *xfbt)
{
	return xfbt->target->bt_xfile;
}

/* Allocate a block to our in-memory btree. */
int
xfbtree_alloc_block(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*start,
	union xfs_btree_ptr		*new,
	int				*stat)
{
	struct xfbtree			*xfbt = cur->bc_mem.xfbtree;
	uint64_t			bt_xfoff;
	loff_t				pos;
	int				error;

	ASSERT(xfs_btree_has_xfile(cur));

	/*
	 * Find the first free block in the free space bitmap and take it.  If
	 * none are found, seek to end of the file.
	 */
	error = bitmap_take_first_set(xfbt->freespace, 0, -1ULL, &bt_xfoff);
	if (error == -ENODATA) {
		bt_xfoff = xfbt->highest_offset++;
		error = 0;
	}
	if (error)
		return error;

	trace_xfbtree_alloc_block(xfbt, cur, bt_xfoff);

	/* Fail if the block address exceeds the maximum for short pointers. */
	if (!xfs_btree_has_long_ptrs(cur) && bt_xfoff >= INT_MAX) {
		*stat = 0;
		return 0;
	}

	/* Make sure we actually can write to the block before we return it. */
	pos = xfo_to_b(bt_xfoff);
	error = xfile_prealloc(xfbtree_xfile(xfbt), pos, xfo_to_b(1));
	if (error)
		return error;

	if (xfs_btree_has_long_ptrs(cur))
		new->l = cpu_to_be64(bt_xfoff);
	else
		new->s = cpu_to_be32(bt_xfoff);

	*stat = 1;
	return 0;
}

/* Free a block from our in-memory btree. */
int
xfbtree_free_block(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;
	xfileoff_t		bt_xfoff, bt_xflen;

	ASSERT(xfs_btree_has_xfile(cur));

	bt_xfoff = xfs_daddr_to_xfot(xfs_buf_daddr(bp));
	bt_xflen = xfs_daddr_to_xfot(bp->b_length);

	trace_xfbtree_free_block(xfbt, cur, bt_xfoff);

	return bitmap_set(xfbt->freespace, bt_xfoff, bt_xflen);
}

/* Return the minimum number of records for a btree block. */
int
xfbtree_get_minrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;

	return xfbt->minrecs[level != 0];
}

/* Return the maximum number of records for a btree block. */
int
xfbtree_get_maxrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	struct xfbtree		*xfbt = cur->bc_mem.xfbtree;

	return xfbt->maxrecs[level != 0];
}

/* If this log item is a buffer item that came from the xfbtree, return it. */
static inline struct xfs_buf *
xfbtree_buf_match(
	struct xfbtree			*xfbt,
	const struct xfs_log_item	*lip)
{
	const struct xfs_buf_log_item	*bli;
	struct xfs_buf			*bp;

	if (lip->li_type != XFS_LI_BUF)
		return NULL;

	bli = container_of(lip, struct xfs_buf_log_item, bli_item);
	bp = bli->bli_buf;
	if (bp->b_target != xfbt->target)
		return NULL;

	return bp;
}

/*
 * Detach this (probably dirty) xfbtree buffer from the transaction by any
 * means necessary.  Returns true if the buffer needs to be written.
 */
STATIC bool
xfbtree_trans_bdetach(
	struct xfs_trans	*tp,
	struct xfs_buf		*bp)
{
	struct xfs_buf_log_item	*bli = bp->b_log_item;
	bool			dirty;

	ASSERT(bli != NULL);

	dirty = bli->bli_flags & (XFS_BLI_DIRTY | XFS_BLI_ORDERED);

	bli->bli_flags &= ~(XFS_BLI_DIRTY | XFS_BLI_ORDERED |
			    XFS_BLI_STALE);
	clear_bit(XFS_LI_DIRTY, &bli->bli_item.li_flags);

	while (bp->b_log_item != NULL)
		libxfs_trans_bdetach(tp, bp);

	return dirty;
}

/*
 * Commit changes to the incore btree immediately by writing all dirty xfbtree
 * buffers to the backing xfile.  This detaches all xfbtree buffers from the
 * transaction, even on failure.  The buffer locks are dropped between the
 * delwri queue and submit, so the caller must synchronize btree access.
 *
 * Normally we'd let the buffers commit with the transaction and get written to
 * the xfile via the log, but online repair stages ephemeral btrees in memory
 * and uses the btree_staging functions to write new btrees to disk atomically.
 * The in-memory btree (and its backing store) are discarded at the end of the
 * repair phase, which means that xfbtree buffers cannot commit with the rest
 * of a transaction.
 *
 * In other words, online repair only needs the transaction to collect buffer
 * pointers and to avoid buffer deadlocks, not to guarantee consistency of
 * updates.
 */
int
xfbtree_trans_commit(
	struct xfbtree		*xfbt,
	struct xfs_trans	*tp)
{
	struct xfs_log_item	*lip, *n;
	bool			corrupt = false;
	bool			tp_dirty = false;

	/*
	 * For each xfbtree buffer attached to the transaction, write the dirty
	 * buffers to the xfile and release them.
	 */
	list_for_each_entry_safe(lip, n, &tp->t_items, li_trans) {
		struct xfs_buf	*bp = xfbtree_buf_match(xfbt, lip);
		bool		dirty;

		if (!bp) {
			if (test_bit(XFS_LI_DIRTY, &lip->li_flags))
				tp_dirty |= true;
			continue;
		}

		trace_xfbtree_trans_commit_buf(xfbt, bp);

		dirty = xfbtree_trans_bdetach(tp, bp);
		if (dirty && !corrupt) {
			xfs_failaddr_t	fa = bp->b_ops->verify_struct(bp);

			/*
			 * Because this btree is ephemeral, validate the buffer
			 * structure before delwri_submit so that we can return
			 * corruption errors to the caller without shutting
			 * down the filesystem.
			 *
			 * If the buffer fails verification, log the failure
			 * but continue walking the transaction items so that
			 * we remove all ephemeral btree buffers.
			 *
			 * Since the userspace buffer cache supports marking
			 * buffers dirty and flushing them later, use this to
			 * reduce the number of writes to the xfile.
			 */
			if (fa) {
				corrupt = true;
				xfs_verifier_error(bp, -EFSCORRUPTED, fa);
			} else {
				libxfs_buf_mark_dirty(bp);
			}
		}

		xfs_buf_relse(bp);
	}

	/*
	 * Reset the transaction's dirty flag to reflect the dirty state of the
	 * log items that are still attached.
	 */
	tp->t_flags = (tp->t_flags & ~XFS_TRANS_DIRTY) |
			(tp_dirty ? XFS_TRANS_DIRTY : 0);

	if (corrupt)
		return -EFSCORRUPTED;
	return 0;
}

/*
 * Cancel changes to the incore btree by detaching all the xfbtree buffers.
 * Changes are not written to the backing store.  This is needed for online
 * repair btrees, which are by nature ephemeral.
 */
void
xfbtree_trans_cancel(
	struct xfbtree		*xfbt,
	struct xfs_trans	*tp)
{
	struct xfs_log_item	*lip, *n;
	bool			tp_dirty = false;

	list_for_each_entry_safe(lip, n, &tp->t_items, li_trans) {
		struct xfs_buf	*bp = xfbtree_buf_match(xfbt, lip);

		if (!bp) {
			if (test_bit(XFS_LI_DIRTY, &lip->li_flags))
				tp_dirty |= true;
			continue;
		}

		trace_xfbtree_trans_cancel_buf(xfbt, bp);

		xfbtree_trans_bdetach(tp, bp);
		xfs_buf_relse(bp);
	}

	/*
	 * Reset the transaction's dirty flag to reflect the dirty state of the
	 * log items that are still attached.
	 */
	tp->t_flags = (tp->t_flags & ~XFS_TRANS_DIRTY) |
			(tp_dirty ? XFS_TRANS_DIRTY : 0);
}
