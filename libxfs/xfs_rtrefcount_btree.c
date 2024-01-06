// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
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
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_alloc.h"
#include "xfs_btree.h"
#include "xfs_btree_staging.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_trace.h"
#include "xfs_cksum.h"
#include "xfs_rtgroup.h"
#include "xfs_rtbitmap.h"

static struct kmem_cache	*xfs_rtrefcountbt_cur_cache;

/*
 * Realtime Reference Count btree.
 *
 * This is a btree used to track the owner(s) of a given extent in the realtime
 * device.  See the comments in xfs_refcount_btree.c for more information.
 *
 * This tree is basically the same as the regular refcount btree except that
 * it's rooted in an inode.
 */

static struct xfs_btree_cur *
xfs_rtrefcountbt_dup_cursor(
	struct xfs_btree_cur	*cur)
{
	struct xfs_btree_cur	*new;

	new = xfs_rtrefcountbt_init_cursor(cur->bc_mp, cur->bc_tp,
			cur->bc_ino.rtg, cur->bc_ino.ip);

	return new;
}

static xfs_failaddr_t
xfs_rtrefcountbt_verify(
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = bp->b_target->bt_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	xfs_failaddr_t		fa;
	int			level;

	if (!xfs_verify_magic(bp, block->bb_magic))
		return __this_address;

	if (!xfs_has_reflink(mp))
		return __this_address;
	fa = xfs_btree_lblock_v5hdr_verify(bp, XFS_RMAP_OWN_UNKNOWN);
	if (fa)
		return fa;
	level = be16_to_cpu(block->bb_level);
	if (level > mp->m_rtrefc_maxlevels)
		return __this_address;

	return xfs_btree_lblock_verify(bp, mp->m_rtrefc_mxr[level != 0]);
}

static void
xfs_rtrefcountbt_read_verify(
	struct xfs_buf	*bp)
{
	xfs_failaddr_t	fa;

	if (!xfs_btree_lblock_verify_crc(bp))
		xfs_verifier_error(bp, -EFSBADCRC, __this_address);
	else {
		fa = xfs_rtrefcountbt_verify(bp);
		if (fa)
			xfs_verifier_error(bp, -EFSCORRUPTED, fa);
	}

	if (bp->b_error)
		trace_xfs_btree_corrupt(bp, _RET_IP_);
}

static void
xfs_rtrefcountbt_write_verify(
	struct xfs_buf	*bp)
{
	xfs_failaddr_t	fa;

	fa = xfs_rtrefcountbt_verify(bp);
	if (fa) {
		trace_xfs_btree_corrupt(bp, _RET_IP_);
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
		return;
	}
	xfs_btree_lblock_calc_crc(bp);

}

const struct xfs_buf_ops xfs_rtrefcountbt_buf_ops = {
	.name			= "xfs_rtrefcountbt",
	.magic			= { 0, cpu_to_be32(XFS_RTREFC_CRC_MAGIC) },
	.verify_read		= xfs_rtrefcountbt_read_verify,
	.verify_write		= xfs_rtrefcountbt_write_verify,
	.verify_struct		= xfs_rtrefcountbt_verify,
};

const struct xfs_btree_ops xfs_rtrefcountbt_ops = {
	.rec_len		= sizeof(struct xfs_refcount_rec),
	.key_len		= sizeof(struct xfs_refcount_key),
	.geom_flags		= XFS_BTGEO_LONG_PTRS | XFS_BTGEO_ROOT_IN_INODE |
				  XFS_BTGEO_CRC_BLOCKS | XFS_BTGEO_IROOT_RECORDS,
	.lru_refs		= XFS_REFC_BTREE_REF,

	.dup_cursor		= xfs_rtrefcountbt_dup_cursor,
	.buf_ops		= &xfs_rtrefcountbt_buf_ops,
};

/* Initialize a new rt refcount btree cursor. */
static struct xfs_btree_cur *
xfs_rtrefcountbt_init_common(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_rtgroup	*rtg,
	struct xfs_inode	*ip)
{
	struct xfs_btree_cur	*cur;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_SHARED | XFS_ILOCK_EXCL));

	cur = xfs_btree_alloc_cursor(mp, tp, XFS_BTNUM_RTREFC,
			&xfs_rtrefcountbt_ops, mp->m_rtrefc_maxlevels,
			xfs_rtrefcountbt_cur_cache);
	cur->bc_statoff = XFS_STATS_CALC_INDEX(xs_refcbt_2);

	cur->bc_ino.ip = ip;
	cur->bc_ino.allocated = 0;
	cur->bc_ino.refc.nr_ops = 0;
	cur->bc_ino.refc.shape_changes = 0;

	cur->bc_ino.rtg = xfs_rtgroup_hold(rtg);
	return cur;
}

/* Allocate a new rt refcount btree cursor. */
struct xfs_btree_cur *
xfs_rtrefcountbt_init_cursor(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_rtgroup	*rtg,
	struct xfs_inode	*ip)
{
	struct xfs_btree_cur	*cur;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);

	cur = xfs_rtrefcountbt_init_common(mp, tp, rtg, ip);
	cur->bc_nlevels = be16_to_cpu(ifp->if_broot->bb_level) + 1;
	cur->bc_ino.forksize = xfs_inode_fork_size(ip, XFS_DATA_FORK);
	cur->bc_ino.whichfork = XFS_DATA_FORK;
	return cur;
}

/* Create a new rt reverse mapping btree cursor with a fake root for staging. */
struct xfs_btree_cur *
xfs_rtrefcountbt_stage_cursor(
	struct xfs_mount	*mp,
	struct xfs_rtgroup	*rtg,
	struct xfs_inode	*ip,
	struct xbtree_ifakeroot	*ifake)
{
	struct xfs_btree_cur	*cur;

	cur = xfs_rtrefcountbt_init_common(mp, NULL, rtg, ip);
	cur->bc_nlevels = ifake->if_levels;
	cur->bc_ino.forksize = ifake->if_fork_size;
	cur->bc_ino.whichfork = -1;
	xfs_btree_stage_ifakeroot(cur, ifake, NULL);
	return cur;
}

/*
 * Install a new rt reverse mapping btree root.  Caller is responsible for
 * invalidating and freeing the old btree blocks.
 */
void
xfs_rtrefcountbt_commit_staged_btree(
	struct xfs_btree_cur	*cur,
	struct xfs_trans	*tp)
{
	struct xbtree_ifakeroot	*ifake = cur->bc_ino.ifake;
	struct xfs_ifork	*ifp;
	int			flags = XFS_ILOG_CORE | XFS_ILOG_DBROOT;

	ASSERT(xfs_btree_is_staging(cur));

	/*
	 * Free any resources hanging off the real fork, then shallow-copy the
	 * staging fork's contents into the real fork to transfer everything
	 * we just built.
	 */
	ifp = xfs_ifork_ptr(cur->bc_ino.ip, XFS_DATA_FORK);
	xfs_idestroy_fork(ifp);
	memcpy(ifp, ifake->if_fork, sizeof(struct xfs_ifork));

	xfs_trans_log_inode(tp, cur->bc_ino.ip, flags);
	xfs_btree_commit_ifakeroot(cur, tp, XFS_DATA_FORK,
			&xfs_rtrefcountbt_ops);
}

/* Calculate number of records in a realtime refcount btree block. */
static inline unsigned int
xfs_rtrefcountbt_block_maxrecs(
	unsigned int		blocklen,
	bool			leaf)
{

	if (leaf)
		return blocklen / sizeof(struct xfs_refcount_rec);
	return blocklen / (sizeof(struct xfs_refcount_key) +
			   sizeof(xfs_rtrefcount_ptr_t));
}

/*
 * Calculate number of records in an refcount btree block.
 */
unsigned int
xfs_rtrefcountbt_maxrecs(
	struct xfs_mount	*mp,
	unsigned int		blocklen,
	bool			leaf)
{
	blocklen -= XFS_RTREFCOUNT_BLOCK_LEN;
	return xfs_rtrefcountbt_block_maxrecs(blocklen, leaf);
}

/* Compute the max possible height for realtime refcount btrees. */
unsigned int
xfs_rtrefcountbt_maxlevels_ondisk(void)
{
	unsigned int		minrecs[2];
	unsigned int		blocklen;

	blocklen = XFS_MIN_CRC_BLOCKSIZE - XFS_BTREE_LBLOCK_CRC_LEN;

	minrecs[0] = xfs_rtrefcountbt_block_maxrecs(blocklen, true) / 2;
	minrecs[1] = xfs_rtrefcountbt_block_maxrecs(blocklen, false) / 2;

	/* We need at most one record for every block in an rt group. */
	return xfs_btree_compute_maxlevels(minrecs, XFS_MAX_RGBLOCKS);
}

int __init
xfs_rtrefcountbt_init_cur_cache(void)
{
	xfs_rtrefcountbt_cur_cache = kmem_cache_create("xfs_rtrefcountbt_cur",
			xfs_btree_cur_sizeof(
					xfs_rtrefcountbt_maxlevels_ondisk()),
			0, 0, NULL);

	if (!xfs_rtrefcountbt_cur_cache)
		return -ENOMEM;
	return 0;
}

void
xfs_rtrefcountbt_destroy_cur_cache(void)
{
	kmem_cache_destroy(xfs_rtrefcountbt_cur_cache);
	xfs_rtrefcountbt_cur_cache = NULL;
}

/* Compute the maximum height of a realtime refcount btree. */
void
xfs_rtrefcountbt_compute_maxlevels(
	struct xfs_mount	*mp)
{
	unsigned int		d_maxlevels, r_maxlevels;

	if (!xfs_has_rtreflink(mp)) {
		mp->m_rtrefc_maxlevels = 0;
		return;
	}

	/*
	 * The realtime refcountbt lives on the data device, which means that
	 * its maximum height is constrained by the size of the data device and
	 * the height required to store one refcount record for each rtextent
	 * in an rt group.
	 */
	d_maxlevels = xfs_btree_space_to_height(mp->m_rtrefc_mnr,
				mp->m_sb.sb_dblocks);
	r_maxlevels = xfs_btree_compute_maxlevels(mp->m_rtrefc_mnr,
				xfs_rtb_to_rtx(mp, mp->m_sb.sb_rgblocks));

	/* Add one level to handle the inode root level. */
	mp->m_rtrefc_maxlevels = min(d_maxlevels, r_maxlevels) + 1;
}
