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
	const struct xfs_owner_info	*oinfo,
	xfs_fsblock_t			alloc_hint)
{
	memset(bkl, 0, sizeof(struct bulkload));
	bkl->sc = sc;
	bkl->oinfo = *oinfo; /* structure copy */
	bkl->alloc_hint = alloc_hint;
	INIT_LIST_HEAD(&bkl->resv_list);
}

/* Initialize accounting resources for staging a new inode fork btree. */
void
bulkload_init_inode(
	struct bulkload			*bkl,
	struct repair_ctx		*sc,
	int				whichfork,
	const struct xfs_owner_info	*oinfo)
{
	bulkload_init_ag(bkl, sc, oinfo, XFS_INO_TO_FSB(sc->mp, sc->ip->i_ino));
	bkl->ifake.if_fork = kmem_cache_zalloc(xfs_ifork_cache, 0);
	bkl->ifake.if_fork_size = xfs_inode_fork_size(sc->ip, whichfork);
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

/* Don't let our allocation hint take us beyond EOFS */
static inline void
bulkload_validate_file_alloc_hint(
	struct bulkload		*bkl)
{
	struct repair_ctx	*sc = bkl->sc;

	if (libxfs_verify_fsbno(sc->mp, bkl->alloc_hint))
		return;

	bkl->alloc_hint = XFS_AGB_TO_FSB(sc->mp, 0, XFS_AGFL_BLOCK(sc->mp) + 1);
}

/* Allocate disk space for our new file-based btree. */
int
bulkload_alloc_file_blocks(
	struct bulkload		*bkl,
	uint64_t		nr_blocks)
{
	struct repair_ctx	*sc = bkl->sc;
	struct xfs_mount	*mp = sc->mp;
	int			error = 0;

	while (nr_blocks > 0) {
		struct xfs_alloc_arg	args = {
			.tp		= sc->tp,
			.mp		= mp,
			.oinfo		= bkl->oinfo,
			.minlen		= 1,
			.maxlen		= nr_blocks,
			.prod		= 1,
			.resv		= XFS_AG_RESV_NONE,
		};
		struct xfs_perag	*pag;
		xfs_agnumber_t		agno;

		bulkload_validate_file_alloc_hint(bkl);

		error = -libxfs_alloc_vextent_start_ag(&args, bkl->alloc_hint);
		if (error)
			return error;
		if (args.fsbno == NULLFSBLOCK)
			return ENOSPC;

		agno = XFS_FSB_TO_AGNO(mp, args.fsbno);

		pag = libxfs_perag_get(mp, agno);
		if (!pag) {
			ASSERT(0);
			return -EFSCORRUPTED;
		}

		error = bulkload_add_blocks(bkl, pag, &args);
		libxfs_perag_put(pag);
		if (error)
			return error;

		nr_blocks -= args.len;
		bkl->alloc_hint = args.fsbno + args.len;

		error = -libxfs_defer_finish(&sc->tp);
		if (error)
			return error;
	}

	return 0;
}

/*
 * Free the unused part of a space extent that was reserved for a new ondisk
 * structure.  Returns the number of EFIs logged or a negative errno.
 */
static inline int
bulkload_free_extent(
	struct bulkload		*bkl,
	struct bulkload_resv	*resv,
	bool			btree_committed)
{
	struct repair_ctx	*sc = bkl->sc;
	xfs_agblock_t		free_agbno = resv->agbno;
	xfs_extlen_t		free_aglen = resv->len;
	xfs_fsblock_t		fsbno;
	int			error;

	if (!btree_committed || resv->used == 0) {
		/*
		 * If we're not committing a new btree or we didn't use the
		 * space reservation, free the entire space extent.
		 */
		goto free;
	}

	/*
	 * We used space and committed the btree.  Remove the written blocks
	 * from the reservation and possibly log a new EFI to free any unused
	 * reservation space.
	 */
	free_agbno += resv->used;
	free_aglen -= resv->used;

	if (free_aglen == 0)
		return 0;

free:
	/*
	 * Use EFIs to free the reservations.  We don't need to use EFIs here
	 * like the kernel, but we'll do it to keep the code matched.
	 */
	fsbno = XFS_AGB_TO_FSB(sc->mp, resv->pag->pag_agno, free_agbno);
	error = -libxfs_free_extent_later(sc->tp, fsbno, free_aglen,
			&bkl->oinfo, XFS_AG_RESV_NONE, true);
	if (error)
		return error;

	return 1;
}

/* Free all the accounting info and disk space we reserved for a new btree. */
static int
bulkload_free(
	struct bulkload		*bkl,
	bool			btree_committed)
{
	struct repair_ctx	*sc = bkl->sc;
	struct bulkload_resv	*resv, *n;
	unsigned int		freed = 0;
	int			error = 0;

	list_for_each_entry_safe(resv, n, &bkl->resv_list, list) {
		int		ret;

		ret = bulkload_free_extent(bkl, resv, btree_committed);
		list_del(&resv->list);
		libxfs_perag_put(resv->pag);
		kfree(resv);

		if (ret < 0) {
			error = ret;
			goto junkit;
		}

		freed += ret;
		if (freed >= XREP_MAX_ITRUNCATE_EFIS) {
			error = -libxfs_defer_finish(&sc->tp);
			if (error)
				goto junkit;
			freed = 0;
		}
	}

	if (freed)
		error = -libxfs_defer_finish(&sc->tp);
junkit:
	/*
	 * If we still have reservations attached to @newbt, cleanup must have
	 * failed and the filesystem is about to go down.  Clean up the incore
	 * reservations.
	 */
	list_for_each_entry_safe(resv, n, &bkl->resv_list, list) {
		list_del(&resv->list);
		libxfs_perag_put(resv->pag);
		kfree(resv);
	}

	if (sc->ip) {
		kmem_cache_free(xfs_ifork_cache, bkl->ifake.if_fork);
		bkl->ifake.if_fork = NULL;
	}

	return error;
}

/*
 * Free all the accounting info and unused disk space allocations after
 * committing a new btree.
 */
int
bulkload_commit(
	struct bulkload		*bkl)
{
	return bulkload_free(bkl, true);
}

/*
 * Free all the accounting info and all of the disk space we reserved for a new
 * btree that we're not going to commit.  We want to try to roll things back
 * cleanly for things like ENOSPC midway through allocation.
 */
void
bulkload_cancel(
	struct bulkload		*bkl)
{
	bulkload_free(bkl, false);
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
