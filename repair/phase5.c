// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "protos.h"
#include "err_protos.h"
#include "dinode.h"
#include "rt.h"
#include "versions.h"
#include "threads.h"
#include "progress.h"
#include "slab.h"
#include "rmap.h"
#include "bulkload.h"
#include "agbtree.h"

/*
 * we maintain the current slice (path from root to leaf)
 * of the btree incore.  when we need a new block, we ask
 * the block allocator for the address of a block on that
 * level, map the block in, and set up the appropriate
 * pointers (child, silbing, etc.) and keys that should
 * point to the new block.
 */
typedef struct bt_stat_level  {
	/*
	 * set in setup_cursor routine and maintained in the tree-building
	 * routines
	 */
	xfs_buf_t		*buf_p;		/* 2 buffer pointers to ... */
	xfs_buf_t		*prev_buf_p;
	xfs_agblock_t		agbno;		/* current block being filled */
	xfs_agblock_t		prev_agbno;	/* previous block */
	/*
	 * set in calculate/init cursor routines for each btree level
	 */
	int			num_recs_tot;	/* # tree recs in level */
	int			num_blocks;	/* # tree blocks in level */
	int			num_recs_pb;	/* num_recs_tot / num_blocks */
	int			modulo;		/* num_recs_tot % num_blocks */
} bt_stat_level_t;

typedef struct bt_status  {
	int			init;		/* cursor set up once? */
	int			num_levels;	/* # of levels in btree */
	xfs_extlen_t		num_tot_blocks;	/* # blocks alloc'ed for tree */
	xfs_extlen_t		num_free_blocks;/* # blocks currently unused */

	xfs_agblock_t		root;		/* root block */
	/*
	 * list of blocks to be used to set up this tree
	 * and pointer to the first unused block on the list
	 */
	xfs_agblock_t		*btree_blocks;		/* block list */
	xfs_agblock_t		*free_btree_blocks;	/* first unused block */
	/*
	 * per-level status info
	 */
	bt_stat_level_t		level[XFS_BTREE_MAXLEVELS];
	uint64_t		owner;		/* owner */
} bt_status_t;

static uint64_t	*sb_icount_ag;		/* allocated inodes per ag */
static uint64_t	*sb_ifree_ag;		/* free inodes per ag */
static uint64_t	*sb_fdblocks_ag;	/* free data blocks per ag */

static int
mk_incore_fstree(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	unsigned int		*num_freeblocks)
{
	int			in_extent;
	int			num_extents;
	xfs_agblock_t		extent_start;
	xfs_extlen_t		extent_len;
	xfs_agblock_t		agbno;
	xfs_agblock_t		ag_end;
	uint			free_blocks;
	xfs_extlen_t		blen;
	int			bstate;

	*num_freeblocks = 0;

	/*
	 * scan the bitmap for the ag looking for continuous
	 * extents of free blocks.  At this point, we know
	 * that blocks in the bitmap are either set to an
	 * "in use" state or set to unknown (0) since the
	 * bmaps were zero'ed in phase 4 and only blocks
	 * being used by inodes, inode bmaps, ag headers,
	 * and the files themselves were put into the bitmap.
	 *
	 */
	ASSERT(agno < mp->m_sb.sb_agcount);

	extent_start = extent_len = 0;
	in_extent = 0;
	num_extents = free_blocks = 0;

	if (agno < mp->m_sb.sb_agcount - 1)
		ag_end = mp->m_sb.sb_agblocks;
	else
		ag_end = mp->m_sb.sb_dblocks -
			(xfs_rfsblock_t)mp->m_sb.sb_agblocks *
                       (mp->m_sb.sb_agcount - 1);

	/*
	 * ok, now find the number of extents, keep track of the
	 * largest extent.
	 */
	for (agbno = 0; agbno < ag_end; agbno += blen) {
		bstate = get_bmap_ext(agno, agbno, ag_end, &blen);
		if (bstate < XR_E_INUSE)  {
			free_blocks += blen;
			if (in_extent == 0)  {
				/*
				 * found the start of a free extent
				 */
				in_extent = 1;
				num_extents++;
				extent_start = agbno;
				extent_len = blen;
			} else  {
				extent_len += blen;
			}
		} else   {
			if (in_extent)  {
				/*
				 * free extent ends here, add extent to the
				 * 2 incore extent (avl-to-be-B+) trees
				 */
				in_extent = 0;
#if defined(XR_BLD_FREE_TRACE) && defined(XR_BLD_ADD_EXTENT)
				fprintf(stderr, "adding extent %u [%u %u]\n",
					agno, extent_start, extent_len);
#endif
				add_bno_extent(agno, extent_start, extent_len);
				add_bcnt_extent(agno, extent_start, extent_len);
				*num_freeblocks += extent_len;
			}
		}
	}
	if (in_extent)  {
		/*
		 * free extent ends here
		 */
#if defined(XR_BLD_FREE_TRACE) && defined(XR_BLD_ADD_EXTENT)
		fprintf(stderr, "adding extent %u [%u %u]\n",
			agno, extent_start, extent_len);
#endif
		add_bno_extent(agno, extent_start, extent_len);
		add_bcnt_extent(agno, extent_start, extent_len);
		*num_freeblocks += extent_len;
	}

	return(num_extents);
}

static xfs_agblock_t
get_next_blockaddr(xfs_agnumber_t agno, int level, bt_status_t *curs)
{
	ASSERT(curs->free_btree_blocks < curs->btree_blocks +
						curs->num_tot_blocks);
	ASSERT(curs->num_free_blocks > 0);

	curs->num_free_blocks--;
	return(*curs->free_btree_blocks++);
}

/*
 * set up the dynamically allocated block allocation data in the btree
 * cursor that depends on the info in the static portion of the cursor.
 * allocates space from the incore bno/bcnt extent trees and sets up
 * the first path up the left side of the tree.  Also sets up the
 * cursor pointer to the btree root.   called by init_freespace_cursor()
 * and init_ino_cursor()
 */
static void
setup_cursor(xfs_mount_t *mp, xfs_agnumber_t agno, bt_status_t *curs)
{
	int			j;
	unsigned int		u;
	xfs_extlen_t		big_extent_len;
	xfs_agblock_t		big_extent_start;
	extent_tree_node_t	*ext_ptr;
	extent_tree_node_t	*bno_ext_ptr;
	xfs_extlen_t		blocks_allocated;
	xfs_agblock_t		*agb_ptr;
	int			error;

	/*
	 * get the number of blocks we need to allocate, then
	 * set up block number array, set the free block pointer
	 * to the first block in the array, and null the array
	 */
	big_extent_len = curs->num_tot_blocks;
	blocks_allocated = 0;

	ASSERT(big_extent_len > 0);

	if ((curs->btree_blocks = malloc(sizeof(xfs_agblock_t)
					* big_extent_len)) == NULL)
		do_error(_("could not set up btree block array\n"));

	agb_ptr = curs->free_btree_blocks = curs->btree_blocks;

	for (j = 0; j < curs->num_free_blocks; j++, agb_ptr++)
		*agb_ptr = NULLAGBLOCK;

	/*
	 * grab the smallest extent and use it up, then get the
	 * next smallest.  This mimics the init_*_cursor code.
	 */
	ext_ptr =  findfirst_bcnt_extent(agno);

	agb_ptr = curs->btree_blocks;

	/*
	 * set up the free block array
	 */
	while (blocks_allocated < big_extent_len)  {
		if (!ext_ptr)
			do_error(
_("error - not enough free space in filesystem\n"));
		/*
		 * use up the extent we've got
		 */
		for (u = 0; u < ext_ptr->ex_blockcount &&
				blocks_allocated < big_extent_len; u++)  {
			ASSERT(agb_ptr < curs->btree_blocks
					+ curs->num_tot_blocks);
			*agb_ptr++ = ext_ptr->ex_startblock + u;
			blocks_allocated++;
		}

		error = rmap_add_ag_rec(mp, agno, ext_ptr->ex_startblock, u,
				curs->owner);
		if (error)
			do_error(_("could not set up btree rmaps: %s\n"),
				strerror(-error));

		/*
		 * if we only used part of this last extent, then we
		 * need only to reset the extent in the extent
		 * trees and we're done
		 */
		if (u < ext_ptr->ex_blockcount)  {
			big_extent_start = ext_ptr->ex_startblock + u;
			big_extent_len = ext_ptr->ex_blockcount - u;

			ASSERT(big_extent_len > 0);

			bno_ext_ptr = find_bno_extent(agno,
						ext_ptr->ex_startblock);
			ASSERT(bno_ext_ptr != NULL);
			get_bno_extent(agno, bno_ext_ptr);
			release_extent_tree_node(bno_ext_ptr);

			ext_ptr = get_bcnt_extent(agno, ext_ptr->ex_startblock,
					ext_ptr->ex_blockcount);
			release_extent_tree_node(ext_ptr);
#ifdef XR_BLD_FREE_TRACE
			fprintf(stderr, "releasing extent: %u [%u %u]\n",
				agno, ext_ptr->ex_startblock,
				ext_ptr->ex_blockcount);
			fprintf(stderr, "blocks_allocated = %d\n",
				blocks_allocated);
#endif

			add_bno_extent(agno, big_extent_start, big_extent_len);
			add_bcnt_extent(agno, big_extent_start, big_extent_len);

			return;
		}
		/*
		 * delete the used-up extent from both extent trees and
		 * find next biggest extent
		 */
#ifdef XR_BLD_FREE_TRACE
		fprintf(stderr, "releasing extent: %u [%u %u]\n",
			agno, ext_ptr->ex_startblock, ext_ptr->ex_blockcount);
#endif
		bno_ext_ptr = find_bno_extent(agno, ext_ptr->ex_startblock);
		ASSERT(bno_ext_ptr != NULL);
		get_bno_extent(agno, bno_ext_ptr);
		release_extent_tree_node(bno_ext_ptr);

		ext_ptr = get_bcnt_extent(agno, ext_ptr->ex_startblock,
				ext_ptr->ex_blockcount);
		ASSERT(ext_ptr != NULL);
		release_extent_tree_node(ext_ptr);

		ext_ptr = findfirst_bcnt_extent(agno);
	}
#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "blocks_allocated = %d\n",
		blocks_allocated);
#endif
}

static void
write_cursor(bt_status_t *curs)
{
	int i;

	for (i = 0; i < curs->num_levels; i++)  {
#if defined(XR_BLD_FREE_TRACE) || defined(XR_BLD_INO_TRACE)
		fprintf(stderr, "writing bt block %u\n", curs->level[i].agbno);
#endif
		if (curs->level[i].prev_buf_p != NULL)  {
			ASSERT(curs->level[i].prev_agbno != NULLAGBLOCK);
#if defined(XR_BLD_FREE_TRACE) || defined(XR_BLD_INO_TRACE)
			fprintf(stderr, "writing bt prev block %u\n",
						curs->level[i].prev_agbno);
#endif
			libxfs_buf_mark_dirty(curs->level[i].prev_buf_p);
			libxfs_buf_relse(curs->level[i].prev_buf_p);
		}
		libxfs_buf_mark_dirty(curs->level[i].buf_p);
		libxfs_buf_relse(curs->level[i].buf_p);
	}
}

static void
finish_cursor(bt_status_t *curs)
{
	ASSERT(curs->num_free_blocks == 0);
	free(curs->btree_blocks);
}

/* Map btnum to buffer ops for the types that need it. */
static const struct xfs_buf_ops *
btnum_to_ops(
	xfs_btnum_t	btnum)
{
	switch (btnum) {
	case XFS_BTNUM_BNO:
		return &xfs_bnobt_buf_ops;
	case XFS_BTNUM_CNT:
		return &xfs_cntbt_buf_ops;
	case XFS_BTNUM_INO:
		return &xfs_inobt_buf_ops;
	case XFS_BTNUM_FINO:
		return &xfs_finobt_buf_ops;
	case XFS_BTNUM_RMAP:
		return &xfs_rmapbt_buf_ops;
	case XFS_BTNUM_REFC:
		return &xfs_refcountbt_buf_ops;
	default:
		ASSERT(0);
		return NULL;
	}
}

/*
 * XXX: yet more code that can be shared with mkfs, growfs.
 */
static void
build_agi(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_ino,
	struct bt_rebuild	*btr_fino)
{
	struct xfs_buf		*agi_buf;
	struct xfs_agi		*agi;
	int			i;
	int			error;

	error = -libxfs_buf_get(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, &agi_buf);
	if (error)
		do_error(_("Cannot grab AG %u AGI buffer, err=%d"),
				agno, error);
	agi_buf->b_ops = &xfs_agi_buf_ops;
	agi = agi_buf->b_addr;
	memset(agi, 0, mp->m_sb.sb_sectsize);

	agi->agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
	agi->agi_versionnum = cpu_to_be32(XFS_AGI_VERSION);
	agi->agi_seqno = cpu_to_be32(agno);
	if (agno < mp->m_sb.sb_agcount - 1)
		agi->agi_length = cpu_to_be32(mp->m_sb.sb_agblocks);
	else
		agi->agi_length = cpu_to_be32(mp->m_sb.sb_dblocks -
			(xfs_rfsblock_t) mp->m_sb.sb_agblocks * agno);
	agi->agi_count = cpu_to_be32(btr_ino->count);
	agi->agi_root = cpu_to_be32(btr_ino->newbt.afake.af_root);
	agi->agi_level = cpu_to_be32(btr_ino->newbt.afake.af_levels);
	agi->agi_freecount = cpu_to_be32(btr_ino->freecount);
	agi->agi_newino = cpu_to_be32(btr_ino->first_agino);
	agi->agi_dirino = cpu_to_be32(NULLAGINO);

	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++)
		agi->agi_unlinked[i] = cpu_to_be32(NULLAGINO);

	if (xfs_sb_version_hascrc(&mp->m_sb))
		platform_uuid_copy(&agi->agi_uuid, &mp->m_sb.sb_meta_uuid);

	if (xfs_sb_version_hasfinobt(&mp->m_sb)) {
		agi->agi_free_root =
				cpu_to_be32(btr_fino->newbt.afake.af_root);
		agi->agi_free_level =
				cpu_to_be32(btr_fino->newbt.afake.af_levels);
	}

	libxfs_buf_mark_dirty(agi_buf);
	libxfs_buf_relse(agi_buf);
}

/* rebuild the refcount tree */

/*
 * we don't have to worry here about how chewing up free extents
 * may perturb things because reflink tree building happens before
 * freespace tree building.
 */
static void
init_refc_cursor(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_status	*btree_curs)
{
	size_t			num_recs;
	int			level;
	struct bt_stat_level	*lptr;
	struct bt_stat_level	*p_lptr;
	xfs_extlen_t		blocks_allocated;

	if (!xfs_sb_version_hasreflink(&mp->m_sb)) {
		memset(btree_curs, 0, sizeof(struct bt_status));
		return;
	}

	lptr = &btree_curs->level[0];
	btree_curs->init = 1;
	btree_curs->owner = XFS_RMAP_OWN_REFC;

	/*
	 * build up statistics
	 */
	num_recs = refcount_record_count(mp, agno);
	if (num_recs == 0) {
		/*
		 * easy corner-case -- no refcount records
		 */
		lptr->num_blocks = 1;
		lptr->modulo = 0;
		lptr->num_recs_pb = 0;
		lptr->num_recs_tot = 0;

		btree_curs->num_levels = 1;
		btree_curs->num_tot_blocks = btree_curs->num_free_blocks = 1;

		setup_cursor(mp, agno, btree_curs);

		return;
	}

	blocks_allocated = lptr->num_blocks = howmany(num_recs,
					mp->m_refc_mxr[0]);

	lptr->modulo = num_recs % lptr->num_blocks;
	lptr->num_recs_pb = num_recs / lptr->num_blocks;
	lptr->num_recs_tot = num_recs;
	level = 1;

	if (lptr->num_blocks > 1)  {
		for (; btree_curs->level[level-1].num_blocks > 1
				&& level < XFS_BTREE_MAXLEVELS;
				level++)  {
			lptr = &btree_curs->level[level];
			p_lptr = &btree_curs->level[level - 1];
			lptr->num_blocks = howmany(p_lptr->num_blocks,
					mp->m_refc_mxr[1]);
			lptr->modulo = p_lptr->num_blocks % lptr->num_blocks;
			lptr->num_recs_pb = p_lptr->num_blocks
					/ lptr->num_blocks;
			lptr->num_recs_tot = p_lptr->num_blocks;

			blocks_allocated += lptr->num_blocks;
		}
	}
	ASSERT(lptr->num_blocks == 1);
	btree_curs->num_levels = level;

	btree_curs->num_tot_blocks = btree_curs->num_free_blocks
			= blocks_allocated;

	setup_cursor(mp, agno, btree_curs);
}

static void
prop_refc_cursor(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_status	*btree_curs,
	xfs_agblock_t		startbno,
	int			level)
{
	struct xfs_btree_block	*bt_hdr;
	struct xfs_refcount_key	*bt_key;
	xfs_refcount_ptr_t	*bt_ptr;
	xfs_agblock_t		agbno;
	struct bt_stat_level	*lptr;
	const struct xfs_buf_ops *ops = btnum_to_ops(XFS_BTNUM_REFC);
	int			error;

	level++;

	if (level >= btree_curs->num_levels)
		return;

	lptr = &btree_curs->level[level];
	bt_hdr = XFS_BUF_TO_BLOCK(lptr->buf_p);

	if (be16_to_cpu(bt_hdr->bb_numrecs) == 0)  {
		/*
		 * this only happens once to initialize the
		 * first path up the left side of the tree
		 * where the agbno's are already set up
		 */
		prop_refc_cursor(mp, agno, btree_curs, startbno, level);
	}

	if (be16_to_cpu(bt_hdr->bb_numrecs) ==
				lptr->num_recs_pb + (lptr->modulo > 0))  {
		/*
		 * write out current prev block, grab us a new block,
		 * and set the rightsib pointer of current block
		 */
#ifdef XR_BLD_INO_TRACE
		fprintf(stderr, " ino prop agbno %d ", lptr->prev_agbno);
#endif
		if (lptr->prev_agbno != NULLAGBLOCK)  {
			ASSERT(lptr->prev_buf_p != NULL);
			libxfs_buf_mark_dirty(lptr->prev_buf_p);
			libxfs_buf_relse(lptr->prev_buf_p);
		}
		lptr->prev_agbno = lptr->agbno;
		lptr->prev_buf_p = lptr->buf_p;
		agbno = get_next_blockaddr(agno, level, btree_curs);

		bt_hdr->bb_u.s.bb_rightsib = cpu_to_be32(agbno);

		error = -libxfs_buf_get(mp->m_dev,
				XFS_AGB_TO_DADDR(mp, agno, agbno),
				XFS_FSB_TO_BB(mp, 1), &lptr->buf_p);
		if (error)
			do_error(_("Cannot grab refcountbt buffer, err=%d"),
					error);
		lptr->agbno = agbno;

		if (lptr->modulo)
			lptr->modulo--;

		/*
		 * initialize block header
		 */
		lptr->buf_p->b_ops = ops;
		bt_hdr = XFS_BUF_TO_BLOCK(lptr->buf_p);
		memset(bt_hdr, 0, mp->m_sb.sb_blocksize);
		libxfs_btree_init_block(mp, lptr->buf_p, XFS_BTNUM_REFC,
					level, 0, agno);

		bt_hdr->bb_u.s.bb_leftsib = cpu_to_be32(lptr->prev_agbno);

		/*
		 * propagate extent record for first extent in new block up
		 */
		prop_refc_cursor(mp, agno, btree_curs, startbno, level);
	}
	/*
	 * add inode info to current block
	 */
	be16_add_cpu(&bt_hdr->bb_numrecs, 1);

	bt_key = XFS_REFCOUNT_KEY_ADDR(bt_hdr,
				    be16_to_cpu(bt_hdr->bb_numrecs));
	bt_ptr = XFS_REFCOUNT_PTR_ADDR(bt_hdr,
				    be16_to_cpu(bt_hdr->bb_numrecs),
				    mp->m_refc_mxr[1]);

	bt_key->rc_startblock = cpu_to_be32(startbno);
	*bt_ptr = cpu_to_be32(btree_curs->level[level-1].agbno);
}

/*
 * rebuilds a refcount btree given a cursor.
 */
static void
build_refcount_tree(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_status	*btree_curs)
{
	xfs_agnumber_t		i;
	xfs_agblock_t		j;
	xfs_agblock_t		agbno;
	struct xfs_btree_block	*bt_hdr;
	struct xfs_refcount_irec	*refc_rec;
	struct xfs_slab_cursor	*refc_cur;
	struct xfs_refcount_rec	*bt_rec;
	struct bt_stat_level	*lptr;
	const struct xfs_buf_ops *ops = btnum_to_ops(XFS_BTNUM_REFC);
	int			numrecs;
	int			level = btree_curs->num_levels;
	int			error;

	for (i = 0; i < level; i++)  {
		lptr = &btree_curs->level[i];

		agbno = get_next_blockaddr(agno, i, btree_curs);
		error = -libxfs_buf_get(mp->m_dev,
				XFS_AGB_TO_DADDR(mp, agno, agbno),
				XFS_FSB_TO_BB(mp, 1), &lptr->buf_p);
		if (error)
			do_error(_("Cannot grab refcountbt buffer, err=%d"),
					error);

		if (i == btree_curs->num_levels - 1)
			btree_curs->root = agbno;

		lptr->agbno = agbno;
		lptr->prev_agbno = NULLAGBLOCK;
		lptr->prev_buf_p = NULL;
		/*
		 * initialize block header
		 */

		lptr->buf_p->b_ops = ops;
		bt_hdr = XFS_BUF_TO_BLOCK(lptr->buf_p);
		memset(bt_hdr, 0, mp->m_sb.sb_blocksize);
		libxfs_btree_init_block(mp, lptr->buf_p, XFS_BTNUM_REFC,
					i, 0, agno);
	}

	/*
	 * run along leaf, setting up records.  as we have to switch
	 * blocks, call the prop_refc_cursor routine to set up the new
	 * pointers for the parent.  that can recurse up to the root
	 * if required.  set the sibling pointers for leaf level here.
	 */
	error = init_refcount_cursor(agno, &refc_cur);
	if (error)
		do_error(
_("Insufficient memory to construct refcount cursor."));
	refc_rec = pop_slab_cursor(refc_cur);
	lptr = &btree_curs->level[0];

	for (i = 0; i < lptr->num_blocks; i++)  {
		numrecs = lptr->num_recs_pb + (lptr->modulo > 0);
		ASSERT(refc_rec != NULL || numrecs == 0);

		/*
		 * block initialization, lay in block header
		 */
		lptr->buf_p->b_ops = ops;
		bt_hdr = XFS_BUF_TO_BLOCK(lptr->buf_p);
		memset(bt_hdr, 0, mp->m_sb.sb_blocksize);
		libxfs_btree_init_block(mp, lptr->buf_p, XFS_BTNUM_REFC,
					0, 0, agno);

		bt_hdr->bb_u.s.bb_leftsib = cpu_to_be32(lptr->prev_agbno);
		bt_hdr->bb_numrecs = cpu_to_be16(numrecs);

		if (lptr->modulo > 0)
			lptr->modulo--;

		if (lptr->num_recs_pb > 0)
			prop_refc_cursor(mp, agno, btree_curs,
					refc_rec->rc_startblock, 0);

		bt_rec = (struct xfs_refcount_rec *)
			  ((char *)bt_hdr + XFS_REFCOUNT_BLOCK_LEN);
		for (j = 0; j < be16_to_cpu(bt_hdr->bb_numrecs); j++) {
			ASSERT(refc_rec != NULL);
			bt_rec[j].rc_startblock =
					cpu_to_be32(refc_rec->rc_startblock);
			bt_rec[j].rc_blockcount =
					cpu_to_be32(refc_rec->rc_blockcount);
			bt_rec[j].rc_refcount = cpu_to_be32(refc_rec->rc_refcount);

			refc_rec = pop_slab_cursor(refc_cur);
		}

		if (refc_rec != NULL)  {
			/*
			 * get next leaf level block
			 */
			if (lptr->prev_buf_p != NULL)  {
#ifdef XR_BLD_RL_TRACE
				fprintf(stderr, "writing refcntbt agbno %u\n",
					lptr->prev_agbno);
#endif
				ASSERT(lptr->prev_agbno != NULLAGBLOCK);
				libxfs_buf_mark_dirty(lptr->prev_buf_p);
				libxfs_buf_relse(lptr->prev_buf_p);
			}
			lptr->prev_buf_p = lptr->buf_p;
			lptr->prev_agbno = lptr->agbno;
			lptr->agbno = get_next_blockaddr(agno, 0, btree_curs);
			bt_hdr->bb_u.s.bb_rightsib = cpu_to_be32(lptr->agbno);

			error = -libxfs_buf_get(mp->m_dev,
					XFS_AGB_TO_DADDR(mp, agno, lptr->agbno),
					XFS_FSB_TO_BB(mp, 1),
					&lptr->buf_p);
			if (error)
				do_error(
	_("Cannot grab refcountbt buffer, err=%d"),
						error);
		}
	}
	free_slab_cursor(&refc_cur);
}

/* Fill the AGFL with any leftover btree bulk loader block reservations. */
static void
fill_agfl(
	struct bulkload		*newbt,
	__be32			*agfl_bnos,
	unsigned int		*agfl_idx)
{
	struct bulkload_resv	*resv, *n;
	struct xfs_mount	*mp = newbt->sc->mp;

	for_each_bulkload_reservation(newbt, resv, n) {
		xfs_agblock_t	bno;

		bno = XFS_FSB_TO_AGBNO(mp, resv->fsbno + resv->used);
		while (resv->used < resv->len &&
		       *agfl_idx < libxfs_agfl_size(mp)) {
			agfl_bnos[(*agfl_idx)++] = cpu_to_be32(bno++);
			resv->used++;
		}
	}
}

/*
 * build both the agf and the agfl for an agno given both
 * btree cursors.
 *
 * XXX: yet more common code that can be shared with mkfs/growfs.
 */
static void
build_agf_agfl(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct bt_rebuild	*btr_bno,
	struct bt_rebuild	*btr_cnt,
	struct bt_rebuild	*btr_rmap,
	struct bt_status	*refcnt_bt)
{
	struct extent_tree_node	*ext_ptr;
	struct xfs_buf		*agf_buf, *agfl_buf;
	unsigned int		agfl_idx;
	struct xfs_agfl		*agfl;
	struct xfs_agf		*agf;
	__be32			*freelist;
	int			error;

	error = -libxfs_buf_get(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGF_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, &agf_buf);
	if (error)
		do_error(_("Cannot grab AG %u AGF buffer, err=%d"),
				agno, error);
	agf_buf->b_ops = &xfs_agf_buf_ops;
	agf = agf_buf->b_addr;
	memset(agf, 0, mp->m_sb.sb_sectsize);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "agf = %p, agf_buf->b_addr = %p\n",
		agf, agf_buf->b_addr);
#endif

	/*
	 * set up fixed part of agf
	 */
	agf->agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
	agf->agf_versionnum = cpu_to_be32(XFS_AGF_VERSION);
	agf->agf_seqno = cpu_to_be32(agno);

	if (agno < mp->m_sb.sb_agcount - 1)
		agf->agf_length = cpu_to_be32(mp->m_sb.sb_agblocks);
	else
		agf->agf_length = cpu_to_be32(mp->m_sb.sb_dblocks -
			(xfs_rfsblock_t) mp->m_sb.sb_agblocks * agno);

	agf->agf_roots[XFS_BTNUM_BNO] =
			cpu_to_be32(btr_bno->newbt.afake.af_root);
	agf->agf_levels[XFS_BTNUM_BNO] =
			cpu_to_be32(btr_bno->newbt.afake.af_levels);
	agf->agf_roots[XFS_BTNUM_CNT] =
			cpu_to_be32(btr_cnt->newbt.afake.af_root);
	agf->agf_levels[XFS_BTNUM_CNT] =
			cpu_to_be32(btr_cnt->newbt.afake.af_levels);
	agf->agf_freeblks = cpu_to_be32(btr_bno->freeblks);

	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		agf->agf_roots[XFS_BTNUM_RMAP] =
				cpu_to_be32(btr_rmap->newbt.afake.af_root);
		agf->agf_levels[XFS_BTNUM_RMAP] =
				cpu_to_be32(btr_rmap->newbt.afake.af_levels);
		agf->agf_rmap_blocks =
				cpu_to_be32(btr_rmap->newbt.afake.af_blocks);
	}

	agf->agf_refcount_root = cpu_to_be32(refcnt_bt->root);
	agf->agf_refcount_level = cpu_to_be32(refcnt_bt->num_levels);
	agf->agf_refcount_blocks = cpu_to_be32(refcnt_bt->num_tot_blocks -
			refcnt_bt->num_free_blocks);

	/*
	 * Count and record the number of btree blocks consumed if required.
	 */
	if (xfs_sb_version_haslazysbcount(&mp->m_sb)) {
		unsigned int blks;
		/*
		 * Don't count the root blocks as they are already
		 * accounted for.
		 */
		blks = btr_bno->newbt.afake.af_blocks +
			btr_cnt->newbt.afake.af_blocks - 2;
		if (xfs_sb_version_hasrmapbt(&mp->m_sb))
			blks += btr_rmap->newbt.afake.af_blocks - 1;
		agf->agf_btreeblks = cpu_to_be32(blks);
#ifdef XR_BLD_FREE_TRACE
		fprintf(stderr, "agf->agf_btreeblks = %u\n",
				be32_to_cpu(agf->agf_btreeblks));
#endif
	}

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "bno root = %u, bcnt root = %u, indices = %u %u\n",
			be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNO]),
			be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNT]),
			XFS_BTNUM_BNO,
			XFS_BTNUM_CNT);
#endif

	if (xfs_sb_version_hascrc(&mp->m_sb))
		platform_uuid_copy(&agf->agf_uuid, &mp->m_sb.sb_meta_uuid);

	/* initialise the AGFL, then fill it if there are blocks left over. */
	error = -libxfs_buf_get(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGFL_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, &agfl_buf);
	if (error)
		do_error(_("Cannot grab AG %u AGFL buffer, err=%d"),
				agno, error);
	agfl_buf->b_ops = &xfs_agfl_buf_ops;
	agfl = XFS_BUF_TO_AGFL(agfl_buf);

	/* setting to 0xff results in initialisation to NULLAGBLOCK */
	memset(agfl, 0xff, mp->m_sb.sb_sectsize);
	freelist = xfs_buf_to_agfl_bno(agfl_buf);
	if (xfs_sb_version_hascrc(&mp->m_sb)) {
		agfl->agfl_magicnum = cpu_to_be32(XFS_AGFL_MAGIC);
		agfl->agfl_seqno = cpu_to_be32(agno);
		platform_uuid_copy(&agfl->agfl_uuid, &mp->m_sb.sb_meta_uuid);
		for (agfl_idx = 0; agfl_idx < libxfs_agfl_size(mp); agfl_idx++)
			freelist[agfl_idx] = cpu_to_be32(NULLAGBLOCK);
	}

	/* Fill the AGFL with leftover blocks or save them for later. */
	agfl_idx = 0;
	freelist = xfs_buf_to_agfl_bno(agfl_buf);
	fill_agfl(&btr_bno->newbt, freelist, &agfl_idx);
	fill_agfl(&btr_cnt->newbt, freelist, &agfl_idx);
	if (xfs_sb_version_hasrmapbt(&mp->m_sb))
		fill_agfl(&btr_rmap->newbt, freelist, &agfl_idx);

	/* Set the AGF counters for the AGFL. */
	if (agfl_idx > 0) {
		agf->agf_flfirst = 0;
		agf->agf_fllast = cpu_to_be32(agfl_idx - 1);
		agf->agf_flcount = cpu_to_be32(agfl_idx);
		rmap_store_agflcount(mp, agno, agfl_idx);

#ifdef XR_BLD_FREE_TRACE
		fprintf(stderr, "writing agfl for ag %u\n", agno);
#endif

	} else  {
		agf->agf_flfirst = 0;
		agf->agf_fllast = cpu_to_be32(libxfs_agfl_size(mp) - 1);
		agf->agf_flcount = 0;
	}

	libxfs_buf_mark_dirty(agfl_buf);
	libxfs_buf_relse(agfl_buf);

	ext_ptr = findbiggest_bcnt_extent(agno);
	agf->agf_longest = cpu_to_be32((ext_ptr != NULL) ?
						ext_ptr->ex_blockcount : 0);

	ASSERT(be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNOi]) !=
		be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNTi]));
	ASSERT(be32_to_cpu(agf->agf_refcount_root) !=
		be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNOi]));
	ASSERT(be32_to_cpu(agf->agf_refcount_root) !=
		be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNTi]));

	libxfs_buf_mark_dirty(agf_buf);
	libxfs_buf_relse(agf_buf);

	/*
	 * now fix up the free list appropriately
	 */
	fix_freelist(mp, agno, true);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "wrote agf for ag %u\n", agno);
#endif
}

/*
 * update the superblock counters, sync the sb version numbers and
 * feature bits to the filesystem, and sync up the on-disk superblock
 * to match the incore superblock.
 */
static void
sync_sb(xfs_mount_t *mp)
{
	xfs_buf_t	*bp;

	bp = libxfs_getsb(mp);
	if (!bp)
		do_error(_("couldn't get superblock\n"));

	mp->m_sb.sb_icount = sb_icount;
	mp->m_sb.sb_ifree = sb_ifree;
	mp->m_sb.sb_fdblocks = sb_fdblocks;
	mp->m_sb.sb_frextents = sb_frextents;

	update_sb_version(mp);

	libxfs_sb_to_disk(bp->b_addr, &mp->m_sb);
	libxfs_buf_mark_dirty(bp);
	libxfs_buf_relse(bp);
}

/*
 * make sure the root and realtime inodes show up allocated
 * even if they've been freed.  they get reinitialized in phase6.
 */
static void
keep_fsinos(xfs_mount_t *mp)
{
	ino_tree_node_t		*irec;
	int			i;

	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino),
			XFS_INO_TO_AGINO(mp, mp->m_sb.sb_rootino));

	for (i = 0; i < 3; i++)
		set_inode_used(irec, i);
}

static void
phase5_func(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct xfs_slab		*lost_fsb)
{
	struct repair_ctx	sc = { .mp = mp, };
	struct bt_rebuild	btr_bno;
	struct bt_rebuild	btr_cnt;
	struct bt_rebuild	btr_ino;
	struct bt_rebuild	btr_fino;
	struct bt_rebuild	btr_rmap;
	bt_status_t		refcnt_btree_curs;
	int			extra_blocks = 0;
	uint			num_freeblocks;
	xfs_agblock_t		num_extents;

	if (verbose)
		do_log(_("        - agno = %d\n"), agno);

	/*
	 * build up incore bno and bcnt extent btrees
	 */
	num_extents = mk_incore_fstree(mp, agno, &num_freeblocks);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of bno extents is %d\n", count_bno_extents(agno));
#endif

	if (num_extents == 0)  {
		/*
		 * XXX - what we probably should do here is pick an inode for
		 * a regular file in the allocation group that has space
		 * allocated and shoot it by traversing the bmap list and
		 * putting all its extents on the incore freespace trees,
		 * clearing the inode, and clearing the in-use bit in the
		 * incore inode tree.  Then try mk_incore_fstree() again.
		 */
		do_error(
_("unable to rebuild AG %u.  Not enough free space in on-disk AG.\n"),
			agno);
	}

	init_ino_cursors(&sc, agno, num_freeblocks, &sb_icount_ag[agno],
			&sb_ifree_ag[agno], &btr_ino, &btr_fino);

	init_rmapbt_cursor(&sc, agno, num_freeblocks, &btr_rmap);

	/*
	 * Set up the btree cursors for the on-disk refcount btrees,
	 * which includes pre-allocating all required blocks.
	 */
	init_refc_cursor(mp, agno, &refcnt_btree_curs);

	num_extents = count_bno_extents_blocks(agno, &num_freeblocks);
	/*
	 * lose two blocks per AG -- the space tree roots are counted as
	 * allocated since the space trees always have roots
	 */
	sb_fdblocks_ag[agno] += num_freeblocks - 2;

	if (num_extents == 0)  {
		/*
		 * XXX - what we probably should do here is pick an inode for
		 * a regular file in the allocation group that has space
		 * allocated and shoot it by traversing the bmap list and
		 * putting all its extents on the incore freespace trees,
		 * clearing the inode, and clearing the in-use bit in the
		 * incore inode tree.  Then try mk_incore_fstree() again.
		 */
		do_error(_("unable to rebuild AG %u.  No free space.\n"), agno);
	}

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of bno extents is %d\n", num_extents);
#endif

	/*
	 * track blocks that we might really lose
	 */
	init_freespace_cursors(&sc, agno, num_freeblocks, &num_extents,
			&extra_blocks, &btr_bno, &btr_cnt);

	/*
	 * freespace btrees live in the "free space" but the filesystem treats
	 * AGFL blocks as allocated since they aren't described by the
	 * freespace trees
	 */

	/*
	 * see if we can fit all the extra blocks into the AGFL
	 */
	extra_blocks = (extra_blocks - libxfs_agfl_size(mp) > 0) ?
			extra_blocks - libxfs_agfl_size(mp) : 0;

	if (extra_blocks > 0)
		sb_fdblocks_ag[agno] -= extra_blocks;

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of bno extents is %d\n", count_bno_extents(agno));
	fprintf(stderr, "# of bcnt extents is %d\n", count_bcnt_extents(agno));
#endif

	build_freespace_btrees(&sc, agno, &btr_bno, &btr_cnt);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "# of free blocks == %d/%d\n", btr_bno.freeblks,
			btr_cnt.freeblks);
#endif
	ASSERT(btr_bno.freeblks == btr_cnt.freeblks);

	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		build_rmap_tree(&sc, agno, &btr_rmap);
		sb_fdblocks_ag[agno] += btr_rmap.newbt.afake.af_blocks - 1;
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb)) {
		build_refcount_tree(mp, agno, &refcnt_btree_curs);
		write_cursor(&refcnt_btree_curs);
	}

	/*
	 * set up agf and agfl
	 */
	build_agf_agfl(mp, agno, &btr_bno, &btr_cnt, &btr_rmap,
			&refcnt_btree_curs);

	build_inode_btrees(&sc, agno, &btr_ino, &btr_fino);

	/* build the agi */
	build_agi(mp, agno, &btr_ino, &btr_fino);

	/*
	 * tear down cursors
	 */
	finish_rebuild(mp, &btr_bno, lost_fsb);
	finish_rebuild(mp, &btr_cnt, lost_fsb);
	finish_rebuild(mp, &btr_ino, lost_fsb);
	if (xfs_sb_version_hasfinobt(&mp->m_sb))
		finish_rebuild(mp, &btr_fino, lost_fsb);
	if (xfs_sb_version_hasrmapbt(&mp->m_sb))
		finish_rebuild(mp, &btr_rmap, lost_fsb);
	if (xfs_sb_version_hasreflink(&mp->m_sb))
		finish_cursor(&refcnt_btree_curs);

	/*
	 * release the incore per-AG bno/bcnt trees so the extent nodes
	 * can be recycled
	 */
	release_agbno_extent_tree(agno);
	release_agbcnt_extent_tree(agno);
	PROG_RPT_INC(prog_rpt_done[agno], 1);
}

/* Inject lost blocks back into the filesystem. */
static int
inject_lost_blocks(
	struct xfs_mount	*mp,
	struct xfs_slab		*lost_fsbs)
{
	struct xfs_trans	*tp = NULL;
	struct xfs_slab_cursor	*cur = NULL;
	xfs_fsblock_t		*fsb;
	int			error;

	error = init_slab_cursor(lost_fsbs, NULL, &cur);
	if (error)
		return error;

	while ((fsb = pop_slab_cursor(cur)) != NULL) {
		error = -libxfs_trans_alloc_rollable(mp, 16, &tp);
		if (error)
			goto out_cancel;

		error = -libxfs_free_extent(tp, *fsb, 1,
				&XFS_RMAP_OINFO_ANY_OWNER, XFS_AG_RESV_NONE);
		if (error)
			goto out_cancel;

		error = -libxfs_trans_commit(tp);
		if (error)
			goto out_cancel;
		tp = NULL;
	}

out_cancel:
	if (tp)
		libxfs_trans_cancel(tp);
	free_slab_cursor(&cur);
	return error;
}

void
phase5(xfs_mount_t *mp)
{
	struct xfs_slab		*lost_fsb;
	xfs_agnumber_t		agno;
	int			error;

	do_log(_("Phase 5 - rebuild AG headers and trees...\n"));
	set_progress_msg(PROG_FMT_REBUILD_AG, (uint64_t)glob_agcount);

#ifdef XR_BLD_FREE_TRACE
	fprintf(stderr, "inobt level 1, maxrec = %d, minrec = %d\n",
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 0),
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 0) / 2);
	fprintf(stderr, "inobt level 0 (leaf), maxrec = %d, minrec = %d\n",
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 1),
		libxfs_inobt_maxrecs(mp, mp->m_sb.sb_blocksize, 1) / 2);
	fprintf(stderr, "xr inobt level 0 (leaf), maxrec = %d\n",
		XR_INOBT_BLOCK_MAXRECS(mp, 0));
	fprintf(stderr, "xr inobt level 1 (int), maxrec = %d\n",
		XR_INOBT_BLOCK_MAXRECS(mp, 1));
	fprintf(stderr, "bnobt level 1, maxrec = %d, minrec = %d\n",
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 0),
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 0) / 2);
	fprintf(stderr, "bnobt level 0 (leaf), maxrec = %d, minrec = %d\n",
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 1),
		libxfs_allocbt_maxrecs(mp, mp->m_sb.sb_blocksize, 1) / 2);
#endif
	/*
	 * make sure the root and realtime inodes show up allocated
	 */
	keep_fsinos(mp);

	/* allocate per ag counters */
	sb_icount_ag = calloc(mp->m_sb.sb_agcount, sizeof(uint64_t));
	if (sb_icount_ag == NULL)
		do_error(_("cannot alloc sb_icount_ag buffers\n"));

	sb_ifree_ag = calloc(mp->m_sb.sb_agcount, sizeof(uint64_t));
	if (sb_ifree_ag == NULL)
		do_error(_("cannot alloc sb_ifree_ag buffers\n"));

	sb_fdblocks_ag = calloc(mp->m_sb.sb_agcount, sizeof(uint64_t));
	if (sb_fdblocks_ag == NULL)
		do_error(_("cannot alloc sb_fdblocks_ag buffers\n"));

	error = init_slab(&lost_fsb, sizeof(xfs_fsblock_t));
	if (error)
		do_error(_("cannot alloc lost block slab\n"));

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++)
		phase5_func(mp, agno, lost_fsb);

	print_final_rpt();

	/* aggregate per ag counters */
	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++)  {
		sb_icount += sb_icount_ag[agno];
		sb_ifree += sb_ifree_ag[agno];
		sb_fdblocks += sb_fdblocks_ag[agno];
	}
	free(sb_icount_ag);
	free(sb_ifree_ag);
	free(sb_fdblocks_ag);

	if (mp->m_sb.sb_rblocks)  {
		do_log(
		_("        - generate realtime summary info and bitmap...\n"));
		rtinit(mp);
		generate_rtinfo(mp, btmcompute, sumcompute);
	}

	do_log(_("        - reset superblock...\n"));

	/*
	 * sync superblock counter and set version bits correctly
	 */
	sync_sb(mp);

	/*
	 * Put the per-AG btree rmap data into the rmapbt now that we've reset
	 * the superblock counters.
	 */
	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		error = rmap_store_ag_btree_rec(mp, agno);
		if (error)
			do_error(
_("unable to add AG %u reverse-mapping data to btree.\n"), agno);
	}

	/*
	 * Put blocks that were unnecessarily reserved for btree
	 * reconstruction back into the filesystem free space data.
	 */
	error = inject_lost_blocks(mp, lost_fsb);
	if (error)
		do_error(_("Unable to reinsert lost blocks into filesystem.\n"));
	free_slab(&lost_fsb);

	bad_ino_btree = 0;

}
