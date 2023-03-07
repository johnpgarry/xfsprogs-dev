// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <libxfs.h>
#include "btree.h"
#include "err_protos.h"
#include "libxlog.h"
#include "incore.h"
#include "globals.h"
#include "dinode.h"
#include "slab.h"
#include "rmap.h"
#include "bulkload.h"
#include "bmap_repair.h"

#define min_t(type, x, y) ( ((type)(x)) > ((type)(y)) ? ((type)(y)) : ((type)(x)) )

/*
 * Inode Fork Block Mapping (BMBT) Repair
 * ======================================
 *
 * Gather all the rmap records for the inode and fork we're fixing, reset the
 * incore fork, then recreate the btree.
 */
struct xrep_bmap {
	/* List of new bmap records. */
	struct xfs_slab		*bmap_records;
	struct xfs_slab_cursor	*bmap_cursor;

	/* New fork. */
	struct bulkload		new_fork_info;
	struct xfs_btree_bload	bmap_bload;

	struct repair_ctx	*sc;

	/* How many blocks did we find allocated to this file? */
	xfs_rfsblock_t		nblocks;

	/* How many bmbt blocks did we find for this fork? */
	xfs_rfsblock_t		old_bmbt_block_count;

	/* Which fork are we fixing? */
	int			whichfork;
};

/* Remember this reverse-mapping as a series of bmap records. */
STATIC int
xrep_bmap_from_rmap(
	struct xrep_bmap	*rb,
	xfs_fileoff_t		startoff,
	xfs_fsblock_t		startblock,
	xfs_filblks_t		blockcount,
	bool			unwritten)
{
	struct xfs_bmbt_rec	rbe;
	struct xfs_bmbt_irec	irec;
	int			error = 0;

	irec.br_startoff = startoff;
	irec.br_startblock = startblock;
	irec.br_state = unwritten ? XFS_EXT_UNWRITTEN : XFS_EXT_NORM;

	do {
		xfs_failaddr_t	fa;

		irec.br_blockcount = min_t(xfs_filblks_t, blockcount,
				XFS_MAX_BMBT_EXTLEN);

		fa = libxfs_bmap_validate_extent(rb->sc->ip, rb->whichfork,
				&irec);
		if (fa)
			return -EFSCORRUPTED;

		libxfs_bmbt_disk_set_all(&rbe, &irec);

		error = slab_add(rb->bmap_records, &rbe);
		if (error)
			return error;

		irec.br_startblock += irec.br_blockcount;
		irec.br_startoff += irec.br_blockcount;
		blockcount -= irec.br_blockcount;
	} while (blockcount > 0);

	return 0;
}

/* Check for any obvious errors or conflicts in the file mapping. */
STATIC int
xrep_bmap_check_fork_rmap(
	struct xrep_bmap		*rb,
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec)
{
	struct repair_ctx		*sc = rb->sc;

	/*
	 * Data extents for rt files are never stored on the data device, but
	 * everything else (xattrs, bmbt blocks) can be.
	 */
	if (XFS_IS_REALTIME_INODE(sc->ip) &&
	    !(rec->rm_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK)))
		return EFSCORRUPTED;

	/* Check that this is within the AG. */
	if (!xfs_verify_agbext(cur->bc_ag.pag, rec->rm_startblock,
				rec->rm_blockcount))
		return EFSCORRUPTED;

	/* No contradictory flags. */
	if ((rec->rm_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK)) &&
	    (rec->rm_flags & XFS_RMAP_UNWRITTEN))
		return EFSCORRUPTED;

	/* Check the file offset range. */
	if (!(rec->rm_flags & XFS_RMAP_BMBT_BLOCK) &&
	    !xfs_verify_fileext(sc->mp, rec->rm_offset, rec->rm_blockcount))
		return EFSCORRUPTED;

	return 0;
}

/* Record extents that belong to this inode's fork. */
STATIC int
xrep_bmap_walk_rmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_bmap		*rb = priv;
	struct xfs_mount		*mp = cur->bc_mp;
	xfs_fsblock_t			fsbno;
	int				error;

	/* Skip extents which are not owned by this inode and fork. */
	if (rec->rm_owner != rb->sc->ip->i_ino)
		return 0;

	error = xrep_bmap_check_fork_rmap(rb, cur, rec);
	if (error)
		return error;

	/*
	 * Record all blocks allocated to this file even if the extent isn't
	 * for the fork we're rebuilding so that we can reset di_nblocks later.
	 */
	rb->nblocks += rec->rm_blockcount;

	/* If this rmap isn't for the fork we want, we're done. */
	if (rb->whichfork == XFS_DATA_FORK &&
	    (rec->rm_flags & XFS_RMAP_ATTR_FORK))
		return 0;
	if (rb->whichfork == XFS_ATTR_FORK &&
	    !(rec->rm_flags & XFS_RMAP_ATTR_FORK))
		return 0;

	fsbno = XFS_AGB_TO_FSB(mp, cur->bc_ag.pag->pag_agno,
			rec->rm_startblock);

	if (rec->rm_flags & XFS_RMAP_BMBT_BLOCK) {
		rb->old_bmbt_block_count += rec->rm_blockcount;
		return 0;
	}

	return xrep_bmap_from_rmap(rb, rec->rm_offset, fsbno,
			rec->rm_blockcount,
			rec->rm_flags & XFS_RMAP_UNWRITTEN);
}

/* Compare two bmap extents. */
static int
xrep_bmap_extent_cmp(
	const void			*a,
	const void			*b)
{
	xfs_fileoff_t			ao;
	xfs_fileoff_t			bo;

	ao = libxfs_bmbt_disk_get_startoff((struct xfs_bmbt_rec *)a);
	bo = libxfs_bmbt_disk_get_startoff((struct xfs_bmbt_rec *)b);

	if (ao > bo)
		return 1;
	else if (ao < bo)
		return -1;
	return 0;
}

/* Scan one AG for reverse mappings that we can turn into extent maps. */
STATIC int
xrep_bmap_scan_ag(
	struct xrep_bmap	*rb,
	struct xfs_perag	*pag)
{
	struct repair_ctx	*sc = rb->sc;
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*agf_bp = NULL;
	struct xfs_btree_cur	*cur;
	int			error;

	error = -libxfs_alloc_read_agf(pag, sc->tp, 0, &agf_bp);
	if (error)
		return error;
	if (!agf_bp)
		return ENOMEM;
	cur = libxfs_rmapbt_init_cursor(mp, sc->tp, agf_bp, pag);
	error = -libxfs_rmap_query_all(cur, xrep_bmap_walk_rmap, rb);
	libxfs_btree_del_cursor(cur, error);
	libxfs_trans_brelse(sc->tp, agf_bp);
	return error;
}

/*
 * Collect block mappings for this fork of this inode and decide if we have
 * enough space to rebuild.  Caller is responsible for cleaning up the list if
 * anything goes wrong.
 */
STATIC int
xrep_bmap_find_mappings(
	struct xrep_bmap	*rb)
{
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	int			error;

	/* Iterate the rmaps for extents. */
	for_each_perag(rb->sc->mp, agno, pag) {
		error = xrep_bmap_scan_ag(rb, pag);
		if (error) {
			libxfs_perag_put(pag);
			return error;
		}
	}

	return 0;
}

/* Retrieve bmap data for bulk load. */
STATIC int
xrep_bmap_get_records(
	struct xfs_btree_cur	*cur,
	unsigned int		idx,
	struct xfs_btree_block	*block,
	unsigned int		nr_wanted,
	void			*priv)
{
	struct xfs_bmbt_rec	*rec;
	struct xfs_bmbt_irec	*irec = &cur->bc_rec.b;
	struct xrep_bmap	*rb = priv;
	union xfs_btree_rec	*block_rec;
	unsigned int		loaded;

	for (loaded = 0; loaded < nr_wanted; loaded++, idx++) {
		rec = pop_slab_cursor(rb->bmap_cursor);
		libxfs_bmbt_disk_get_all(rec, irec);

		block_rec = libxfs_btree_rec_addr(cur, idx, block);
		cur->bc_ops->init_rec_from_cur(cur, block_rec);
	}

	return loaded;
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_bmap_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_bmap        *rb = priv;

	return bulkload_claim_block(cur, &rb->new_fork_info, ptr);
}

/* Figure out how much space we need to create the incore btree root block. */
STATIC size_t
xrep_bmap_iroot_size(
	struct xfs_btree_cur	*cur,
	unsigned int		level,
	unsigned int		nr_this_level,
	void			*priv)
{
	ASSERT(level > 0);

	return xfs_bmap_broot_space_calc(cur->bc_mp, nr_this_level);
}

/* Update the inode counters. */
STATIC int
xrep_bmap_reset_counters(
	struct xrep_bmap	*rb)
{
	struct repair_ctx	*sc = rb->sc;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	int64_t			delta;

	/*
	 * Update the inode block counts to reflect the extents we found in the
	 * rmapbt.
	 */
	delta = ifake->if_blocks - rb->old_bmbt_block_count;
	sc->ip->i_nblocks = rb->nblocks + delta;
	libxfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);

	/* Quotas don't exist so we're done. */
	return 0;
}

/*
 * Ensure that the inode being repaired is ready to handle a certain number of
 * extents, or return EFSCORRUPTED.  Caller must hold the ILOCK of the inode
 * being repaired and have joined it to the scrub transaction.
 */
static int
xrep_ino_ensure_extent_count(
	struct repair_ctx	*sc,
	int			whichfork,
	xfs_extnum_t		nextents)
{
	xfs_extnum_t		max_extents;
	bool			large_extcount;

	large_extcount = xfs_inode_has_large_extent_counts(sc->ip);
	max_extents = xfs_iext_max_nextents(large_extcount, whichfork);
	if (nextents <= max_extents)
		return 0;
	if (large_extcount)
		return EFSCORRUPTED;
	if (!xfs_has_large_extent_counts(sc->mp))
		return EFSCORRUPTED;

	max_extents = xfs_iext_max_nextents(true, whichfork);
	if (nextents > max_extents)
		return EFSCORRUPTED;

	sc->ip->i_diflags2 |= XFS_DIFLAG2_NREXT64;
	libxfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);
	return 0;
}

/*
 * Create a new iext tree and load it with block mappings.  If the inode is
 * in extents format, that's all we need to do to commit the new mappings.
 * If it is in btree format, this takes care of preloading the incore tree.
 */
STATIC int
xrep_bmap_extents_load(
	struct xrep_bmap	*rb,
	struct xfs_btree_cur	*bmap_cur,
	uint64_t		nextents)
{
	struct xfs_iext_cursor	icur;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	struct xfs_ifork	*ifp = ifake->if_fork;
	unsigned int		i;
	int			error;

	ASSERT(ifp->if_bytes == 0);

	error = init_slab_cursor(rb->bmap_records, xrep_bmap_extent_cmp,
			&rb->bmap_cursor);
	if (error)
		return error;

	/* Add all the mappings to the incore extent tree. */
	libxfs_iext_first(ifp, &icur);
	for (i = 0; i < nextents; i++) {
		struct xfs_bmbt_rec	*rec;

		rec = pop_slab_cursor(rb->bmap_cursor);
		libxfs_bmbt_disk_get_all(rec, &bmap_cur->bc_rec.b);
		libxfs_iext_insert_raw(ifp, &icur, &bmap_cur->bc_rec.b);
		ifp->if_nextents++;
		libxfs_iext_next(ifp, &icur);
	}
	free_slab_cursor(&rb->bmap_cursor);

	return xrep_ino_ensure_extent_count(rb->sc, rb->whichfork,
			ifp->if_nextents);
}

/*
 * Reserve new btree blocks, bulk load the bmap records into the ondisk btree,
 * and load the incore extent tree.
 */
STATIC int
xrep_bmap_btree_load(
	struct xrep_bmap	*rb,
	struct xfs_btree_cur	*bmap_cur,
	uint64_t		nextents)
{
	struct repair_ctx	*sc = rb->sc;
	int			error;

	rb->bmap_bload.get_records = xrep_bmap_get_records;
	rb->bmap_bload.claim_block = xrep_bmap_claim_block;
	rb->bmap_bload.iroot_size = xrep_bmap_iroot_size;
	rb->bmap_bload.max_dirty = XFS_B_TO_FSBT(sc->mp, 256U << 10); /* 256K */

	/*
	 * Always make the btree as small as possible, since we might need the
	 * space to rebuild the space metadata btrees in later phases.
	 */
	rb->bmap_bload.leaf_slack = 0;
	rb->bmap_bload.node_slack = 0;

	/* Compute how many blocks we'll need. */
	error = -libxfs_btree_bload_compute_geometry(bmap_cur, &rb->bmap_bload,
			nextents);
	if (error)
		return error;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire bmap
	 * from the number of extents we found, and pump up our transaction to
	 * have sufficient block reservation.
	 */
	error = -libxfs_trans_reserve_more(sc->tp, rb->bmap_bload.nr_blocks, 0);
	if (error)
		return error;

	/* Reserve the space we'll need for the new btree. */
	error = bulkload_alloc_file_blocks(&rb->new_fork_info,
			rb->bmap_bload.nr_blocks);
	if (error)
		return error;

	/* Add all observed bmap records. */
	error = init_slab_cursor(rb->bmap_records, xrep_bmap_extent_cmp,
			&rb->bmap_cursor);
	if (error)
		return error;
	error = -libxfs_btree_bload(bmap_cur, &rb->bmap_bload, rb);
	free_slab_cursor(&rb->bmap_cursor);
	if (error)
	       return error;

	/*
	 * Load the new bmap records into the new incore extent tree to
	 * preserve delalloc reservations for regular files.  The directory
	 * code loads the extent tree during xfs_dir_open and assumes
	 * thereafter that it remains loaded, so we must not violate that
	 * assumption.
	 */
	return xrep_bmap_extents_load(rb, bmap_cur, nextents);
}

/*
 * Use the collected bmap information to stage a new bmap fork.  If this is
 * successful we'll return with the new fork information logged to the repair
 * transaction but not yet committed.
 */
STATIC int
xrep_bmap_build_new_fork(
	struct xrep_bmap	*rb)
{
	struct xfs_owner_info	oinfo;
	struct repair_ctx	*sc = rb->sc;
	struct xfs_btree_cur	*bmap_cur;
	struct xbtree_ifakeroot	*ifake = &rb->new_fork_info.ifake;
	uint64_t		nextents;
	int			error;

	/*
	 * Sort the bmap extents by startblock to avoid btree splits when we
	 * rebuild the bmbt btree.
	 */
	qsort_slab(rb->bmap_records, xrep_bmap_extent_cmp);

	/*
	 * Prepare to construct the new fork by initializing the new btree
	 * structure and creating a fake ifork in the ifakeroot structure.
	 */
	libxfs_rmap_ino_bmbt_owner(&oinfo, sc->ip->i_ino, rb->whichfork);
	bulkload_init_inode(&rb->new_fork_info, sc, rb->whichfork, &oinfo);
	bmap_cur = libxfs_bmbt_stage_cursor(sc->mp, sc->ip, ifake);

	/*
	 * Figure out the size and format of the new fork, then fill it with
	 * all the bmap records we've found.  Join the inode to the transaction
	 * so that we can roll the transaction while holding the inode locked.
	 */
	libxfs_trans_ijoin(sc->tp, sc->ip, 0);
	nextents = slab_count(rb->bmap_records);
	if (nextents <= XFS_IFORK_MAXEXT(sc->ip, rb->whichfork)) {
		ifake->if_fork->if_format = XFS_DINODE_FMT_EXTENTS;
		error = xrep_bmap_extents_load(rb, bmap_cur, nextents);
	} else {
		ifake->if_fork->if_format = XFS_DINODE_FMT_BTREE;
		error = xrep_bmap_btree_load(rb, bmap_cur, nextents);
	}
	if (error)
		goto err_cur;

	/*
	 * Install the new fork in the inode.  After this point the old mapping
	 * data are no longer accessible and the new tree is live.  We delete
	 * the cursor immediately after committing the staged root because the
	 * staged fork might be in extents format.
	 */
	libxfs_bmbt_commit_staged_btree(bmap_cur, sc->tp, rb->whichfork);
	libxfs_btree_del_cursor(bmap_cur, 0);

	/* Reset the inode counters now that we've changed the fork. */
	error = xrep_bmap_reset_counters(rb);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting infomation. */
	error = bulkload_commit(&rb->new_fork_info);
	if (error)
		return error;

	return -libxfs_trans_roll_inode(&sc->tp, sc->ip);
err_cur:
	if (bmap_cur)
		libxfs_btree_del_cursor(bmap_cur, error);
err_newbt:
	bulkload_cancel(&rb->new_fork_info);
	return error;
}

/* Check for garbage inputs.  Returns ECANCELED if there's nothing to do. */
STATIC int
xrep_bmap_check_inputs(
	struct repair_ctx	*sc,
	int			whichfork)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(sc->ip, whichfork);

	ASSERT(whichfork == XFS_DATA_FORK || whichfork == XFS_ATTR_FORK);

	if (!xfs_has_rmapbt(sc->mp))
		return EOPNOTSUPP;

	/* No fork means nothing to rebuild. */
	if (!ifp)
		return ECANCELED;

	/*
	 * We only know how to repair extent mappings, which is to say that we
	 * only support extents and btree fork format.  Repairs to a local
	 * format fork require a higher level repair function, so we do not
	 * have any work to do here.
	 */
	switch (ifp->if_format) {
	case XFS_DINODE_FMT_DEV:
	case XFS_DINODE_FMT_LOCAL:
	case XFS_DINODE_FMT_UUID:
		return ECANCELED;
	case XFS_DINODE_FMT_EXTENTS:
	case XFS_DINODE_FMT_BTREE:
		break;
	default:
		return EFSCORRUPTED;
	}

	if (whichfork == XFS_ATTR_FORK)
		return 0;

	/* Only files, symlinks, and directories get to have data forks. */
	switch (VFS_I(sc->ip)->i_mode & S_IFMT) {
	case S_IFREG:
	case S_IFDIR:
	case S_IFLNK:
		/* ok */
		break;
	default:
		return EINVAL;
	}

	/* Don't know how to rebuild realtime data forks. */
	if (XFS_IS_REALTIME_INODE(sc->ip))
		return EOPNOTSUPP;

	return 0;
}

/* Repair an inode fork. */
STATIC int
xrep_bmap(
	struct repair_ctx	*sc,
	int			whichfork)
{
	struct xrep_bmap	*rb;
	int			error = 0;

	error = xrep_bmap_check_inputs(sc, whichfork);
	if (error == ECANCELED)
		return 0;
	if (error)
		return error;

	rb = kmem_zalloc(sizeof(struct xrep_bmap), KM_NOFS | KM_MAYFAIL);
	if (!rb)
		return ENOMEM;
	rb->sc = sc;
	rb->whichfork = whichfork;

	/* Set up some storage */
	error = init_slab(&rb->bmap_records, sizeof(struct xfs_bmbt_rec));
	if (error)
		goto out_rb;

	/* Collect all reverse mappings for this fork's extents. */
	error = xrep_bmap_find_mappings(rb);
	if (error)
		goto out_bitmap;

	/* Rebuild the bmap information. */
	error = xrep_bmap_build_new_fork(rb);

	/*
	 * We don't need to free the old bmbt blocks because we're rebuilding
	 * all the space metadata later.
	 */

out_bitmap:
	free_slab(&rb->bmap_records);
out_rb:
	kmem_free(rb);
	return error;
}

/* Rebuild some inode's bmap. */
int
rebuild_bmap(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	int			whichfork,
	unsigned long		nr_extents,
	struct xfs_buf		**ino_bpp,
	struct xfs_dinode	**dinop,
	int			*dirty)
{
	struct repair_ctx	sc = {
		.mp		= mp,
	};
	const struct xfs_buf_ops *bp_ops;
	unsigned long		boffset;
	unsigned long long	resblks;
	xfs_daddr_t		bp_bn;
	int			bp_length;
	int			error, err2;

	bp_bn = xfs_buf_daddr(*ino_bpp);
	bp_length = (*ino_bpp)->b_length;
	bp_ops = (*ino_bpp)->b_ops;
	boffset = (char *)(*dinop) - (char *)(*ino_bpp)->b_addr;

	/*
	 * Bail out if the inode didn't think it had extents.  Otherwise, zap
	 * it back to a zero-extents fork so that we can rebuild it.
	 */
	switch (whichfork) {
	case XFS_DATA_FORK:
		if ((*dinop)->di_nextents == 0)
			return 0;
		(*dinop)->di_format = XFS_DINODE_FMT_EXTENTS;
		(*dinop)->di_nextents = 0;
		libxfs_dinode_calc_crc(mp, *dinop);
		*dirty = 1;
		break;
	case XFS_ATTR_FORK:
		if ((*dinop)->di_anextents == 0)
			return 0;
		(*dinop)->di_aformat = XFS_DINODE_FMT_EXTENTS;
		(*dinop)->di_anextents = 0;
		libxfs_dinode_calc_crc(mp, *dinop);
		*dirty = 1;
		break;
	default:
		return EINVAL;
	}

	resblks = libxfs_bmbt_calc_size(mp, nr_extents);
	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, resblks, 0,
			0, &sc.tp);
	if (error)
		return error;

	/*
	 * Repair magic: the caller passed us the inode cluster buffer for the
	 * inode.  The _iget call grabs the buffer to load the incore inode, so
	 * the buffer must be attached to the transaction to avoid recursing
	 * the buffer lock.
	 *
	 * Unfortunately, the _iget call drops the buffer once the inode is
	 * loaded, so if we've made any changes we have to log the buffer, hold
	 * it, and roll the transaction.  This persists the caller's changes
	 * and maintains our ownership of the cluster buffer.
	 */
	libxfs_trans_bjoin(sc.tp, *ino_bpp);
	if (*dirty) {
		unsigned int	end = BBTOB((*ino_bpp)->b_length) - 1;

		libxfs_trans_log_buf(sc.tp, *ino_bpp, 0, end);
		*dirty = 0;

		libxfs_trans_bhold(sc.tp, *ino_bpp);
		error = -libxfs_trans_roll(&sc.tp);
		libxfs_trans_bjoin(sc.tp, *ino_bpp);
		if (error)
			goto out_cancel;
	}

	/* Grab the inode and fix the bmbt. */
	error = -libxfs_iget(mp, sc.tp, ino, 0, &sc.ip);
	if (error)
		goto out_cancel;
	error = xrep_bmap(&sc, whichfork);
	if (error)
		libxfs_trans_cancel(sc.tp);
	else
		error = -libxfs_trans_commit(sc.tp);

	/*
	 * Rebuilding the inode fork rolled the transaction, so we need to
	 * re-grab the inode cluster buffer and dinode pointer for the caller.
	 */
	err2 = -libxfs_imap_to_bp(mp, NULL, &sc.ip->i_imap, ino_bpp);
	if (err2)
		do_error(
 _("Unable to re-grab inode cluster buffer after failed repair of inode %llu, error %d.\n"),
				(unsigned long long)ino, err2);
	*dinop = xfs_buf_offset(*ino_bpp, sc.ip->i_imap.im_boffset);
	libxfs_irele(sc.ip);

	return error;

out_cancel:
	libxfs_trans_cancel(sc.tp);

	/*
	 * Try to regrab the old buffer so we have something to return to the
	 * caller.
	 */
	err2 = -libxfs_trans_read_buf(mp, NULL, mp->m_ddev_targp, bp_bn,
			bp_length, 0, ino_bpp, bp_ops);
	if (err2)
		do_error(
 _("Unable to re-grab inode cluster buffer after failed repair of inode %llu, error %d.\n"),
				(unsigned long long)ino, err2);
	*dinop = xfs_buf_offset(*ino_bpp, boffset);
	return error;
}
