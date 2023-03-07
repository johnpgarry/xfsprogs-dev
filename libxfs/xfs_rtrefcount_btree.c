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
#include "xfs_refcount.h"
#include "xfs_trace.h"
#include "xfs_cksum.h"
#include "xfs_rtgroup.h"
#include "xfs_rtbitmap.h"
#include "xfs_imeta.h"

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

STATIC int
xfs_rtrefcountbt_get_minrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	if (level == cur->bc_nlevels - 1) {
		struct xfs_ifork	*ifp = xfs_btree_ifork_ptr(cur);

		return xfs_rtrefcountbt_maxrecs(cur->bc_mp, ifp->if_broot_bytes,
				level == 0) / 2;
	}

	return cur->bc_mp->m_rtrefc_mnr[level != 0];
}

STATIC int
xfs_rtrefcountbt_get_maxrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	if (level == cur->bc_nlevels - 1) {
		struct xfs_ifork	*ifp = xfs_btree_ifork_ptr(cur);

		return xfs_rtrefcountbt_maxrecs(cur->bc_mp, ifp->if_broot_bytes,
				level == 0);
	}

	return cur->bc_mp->m_rtrefc_mxr[level != 0];
}

/*
 * Calculate number of records in a realtime refcount btree inode root.
 */
unsigned int
xfs_rtrefcountbt_droot_maxrecs(
	unsigned int		blocklen,
	bool			leaf)
{
	blocklen -= sizeof(struct xfs_rtrefcount_root);

	if (leaf)
		return blocklen / sizeof(struct xfs_refcount_rec);
	return blocklen / (2 * sizeof(struct xfs_refcount_key) +
			sizeof(xfs_rtrefcount_ptr_t));
}

/*
 * Get the maximum records we could store in the on-disk format.
 *
 * For non-root nodes this is equivalent to xfs_rtrefcountbt_get_maxrecs, but
 * for the root node this checks the available space in the dinode fork so that
 * we can resize the in-memory buffer to match it.  After a resize to the
 * maximum size this function returns the same value as
 * xfs_rtrefcountbt_get_maxrecs for the root node, too.
 */
STATIC int
xfs_rtrefcountbt_get_dmaxrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	if (level != cur->bc_nlevels - 1)
		return cur->bc_mp->m_rtrefc_mxr[level != 0];
	return xfs_rtrefcountbt_droot_maxrecs(cur->bc_ino.forksize, level == 0);
}

STATIC void
xfs_rtrefcountbt_init_key_from_rec(
	union xfs_btree_key		*key,
	const union xfs_btree_rec	*rec)
{
	key->refc.rc_startblock = rec->refc.rc_startblock;
}

STATIC void
xfs_rtrefcountbt_init_high_key_from_rec(
	union xfs_btree_key		*key,
	const union xfs_btree_rec	*rec)
{
	__u32				x;

	x = be32_to_cpu(rec->refc.rc_startblock);
	x += be32_to_cpu(rec->refc.rc_blockcount) - 1;
	key->refc.rc_startblock = cpu_to_be32(x);
}

STATIC void
xfs_rtrefcountbt_init_rec_from_cur(
	struct xfs_btree_cur	*cur,
	union xfs_btree_rec	*rec)
{
	const struct xfs_refcount_irec *irec = &cur->bc_rec.rc;
	uint32_t		start;

	start = xfs_refcount_encode_startblock(irec->rc_startblock,
			irec->rc_domain);
	rec->refc.rc_startblock = cpu_to_be32(start);
	rec->refc.rc_blockcount = cpu_to_be32(cur->bc_rec.rc.rc_blockcount);
	rec->refc.rc_refcount = cpu_to_be32(cur->bc_rec.rc.rc_refcount);
}

STATIC void
xfs_rtrefcountbt_init_ptr_from_cur(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr)
{
	ptr->l = 0;
}

STATIC int64_t
xfs_rtrefcountbt_key_diff(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key)
{
	const struct xfs_refcount_key	*kp = &key->refc;
	const struct xfs_refcount_irec	*irec = &cur->bc_rec.rc;
	uint32_t			start;

	start = xfs_refcount_encode_startblock(irec->rc_startblock,
			irec->rc_domain);
	return (int64_t)be32_to_cpu(kp->rc_startblock) - start;
}

STATIC int64_t
xfs_rtrefcountbt_diff_two_keys(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*k1,
	const union xfs_btree_key	*k2,
	const union xfs_btree_key	*mask)
{
	ASSERT(!mask || mask->refc.rc_startblock);

	return (int64_t)be32_to_cpu(k1->refc.rc_startblock) -
			be32_to_cpu(k2->refc.rc_startblock);
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

STATIC int
xfs_rtrefcountbt_keys_inorder(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*k1,
	const union xfs_btree_key	*k2)
{
	return be32_to_cpu(k1->refc.rc_startblock) <
	       be32_to_cpu(k2->refc.rc_startblock);
}

STATIC int
xfs_rtrefcountbt_recs_inorder(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_rec	*r1,
	const union xfs_btree_rec	*r2)
{
	return  be32_to_cpu(r1->refc.rc_startblock) +
		be32_to_cpu(r1->refc.rc_blockcount) <=
		be32_to_cpu(r2->refc.rc_startblock);
}

STATIC enum xbtree_key_contig
xfs_rtrefcountbt_keys_contiguous(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2,
	const union xfs_btree_key	*mask)
{
	ASSERT(!mask || mask->refc.rc_startblock);

	return xbtree_key_contig(be32_to_cpu(key1->refc.rc_startblock),
				 be32_to_cpu(key2->refc.rc_startblock));
}

/* Move the rt refcount btree root from one incore buffer to another. */
static void
xfs_rtrefcountbt_broot_move(
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_btree_block	*dst_broot,
	size_t			dst_bytes,
	struct xfs_btree_block	*src_broot,
	size_t			src_bytes,
	unsigned int		level,
	unsigned int		numrecs)
{
	struct xfs_mount	*mp = ip->i_mount;
	void			*dptr;
	void			*sptr;

	ASSERT(xfs_rtrefcount_droot_space(src_broot) <=
			xfs_inode_fork_size(ip, whichfork));

	/*
	 * We always have to move the pointers because they are not butted
	 * against the btree block header.
	 */
	if (numrecs && level > 0) {
		sptr = xfs_rtrefcount_broot_ptr_addr(mp, src_broot, 1,
				src_bytes);
		dptr = xfs_rtrefcount_broot_ptr_addr(mp, dst_broot, 1,
				dst_bytes);
		memmove(dptr, sptr, numrecs * sizeof(xfs_fsblock_t));
	}

	if (src_broot == dst_broot)
		return;

	/*
	 * If the root is being totally relocated, we have to migrate the block
	 * header and the keys/records that come after it.
	 */
	memcpy(dst_broot, src_broot, XFS_RTREFCOUNT_BLOCK_LEN);

	if (!numrecs)
		return;

	if (level == 0) {
		sptr = xfs_rtrefcount_rec_addr(src_broot, 1);
		dptr = xfs_rtrefcount_rec_addr(dst_broot, 1);
		memcpy(dptr, sptr,
				numrecs * sizeof(struct xfs_refcount_rec));
	} else {
		sptr = xfs_rtrefcount_key_addr(src_broot, 1);
		dptr = xfs_rtrefcount_key_addr(dst_broot, 1);
		memcpy(dptr, sptr,
				numrecs * sizeof(struct xfs_refcount_key));
	}
}

static const struct xfs_ifork_broot_ops xfs_rtrefcountbt_iroot_ops = {
	.maxrecs		= xfs_rtrefcountbt_maxrecs,
	.size			= xfs_rtrefcount_broot_space_calc,
	.move			= xfs_rtrefcountbt_broot_move,
};

const struct xfs_btree_ops xfs_rtrefcountbt_ops = {
	.rec_len		= sizeof(struct xfs_refcount_rec),
	.key_len		= sizeof(struct xfs_refcount_key),
	.geom_flags		= XFS_BTGEO_LONG_PTRS | XFS_BTGEO_ROOT_IN_INODE |
				  XFS_BTGEO_CRC_BLOCKS | XFS_BTGEO_IROOT_RECORDS,
	.lru_refs		= XFS_REFC_BTREE_REF,

	.dup_cursor		= xfs_rtrefcountbt_dup_cursor,
	.alloc_block		= xfs_btree_alloc_imeta_block,
	.free_block		= xfs_btree_free_imeta_block,
	.get_minrecs		= xfs_rtrefcountbt_get_minrecs,
	.get_maxrecs		= xfs_rtrefcountbt_get_maxrecs,
	.get_dmaxrecs		= xfs_rtrefcountbt_get_dmaxrecs,
	.init_key_from_rec	= xfs_rtrefcountbt_init_key_from_rec,
	.init_high_key_from_rec	= xfs_rtrefcountbt_init_high_key_from_rec,
	.init_rec_from_cur	= xfs_rtrefcountbt_init_rec_from_cur,
	.init_ptr_from_cur	= xfs_rtrefcountbt_init_ptr_from_cur,
	.key_diff		= xfs_rtrefcountbt_key_diff,
	.buf_ops		= &xfs_rtrefcountbt_buf_ops,
	.diff_two_keys		= xfs_rtrefcountbt_diff_two_keys,
	.keys_inorder		= xfs_rtrefcountbt_keys_inorder,
	.recs_inorder		= xfs_rtrefcountbt_recs_inorder,
	.keys_contiguous	= xfs_rtrefcountbt_keys_contiguous,
	.iroot_ops		= &xfs_rtrefcountbt_iroot_ops,
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
	ASSERT(ifake->if_fork->if_format == XFS_DINODE_FMT_REFCOUNT);

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

#define XFS_RTREFC_NAMELEN		21

/* Create the metadata directory path for an rtrefcount btree inode. */
int
xfs_rtrefcountbt_create_path(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno,
	struct xfs_imeta_path	**pathp)
{
	struct xfs_imeta_path	*path;
	unsigned char		*fname;
	int			error;

	error = xfs_imeta_create_file_path(mp, 2, &path);
	if (error)
		return error;

	fname = kmalloc(XFS_RTREFC_NAMELEN, GFP_KERNEL);
	if (!fname) {
		xfs_imeta_free_path(path);
		return -ENOMEM;
	}

	snprintf(fname, XFS_RTREFC_NAMELEN, "%u.refcount", rgno);
	path->im_path[0] = "realtime";
	path->im_path[1] = fname;
	path->im_dynamicmask = 0x2;
	*pathp = path;
	return 0;
}

/* Calculate the rtrefcount btree size for some records. */
unsigned long long
xfs_rtrefcountbt_calc_size(
	struct xfs_mount	*mp,
	unsigned long long	len)
{
	return xfs_btree_calc_size(mp->m_rtrefc_mnr, len);
}

/*
 * Calculate the maximum refcount btree size.
 */
static unsigned long long
xfs_rtrefcountbt_max_size(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtblocks)
{
	/* Bail out if we're uninitialized, which can happen in mkfs. */
	if (mp->m_rtrefc_mxr[0] == 0)
		return 0;

	return xfs_rtrefcountbt_calc_size(mp, rtblocks);
}

/*
 * Figure out how many blocks to reserve and how many are used by this btree.
 * We need enough space to hold one record for every rt extent in the rtgroup.
 */
xfs_filblks_t
xfs_rtrefcountbt_calc_reserves(
	struct xfs_mount	*mp)
{
	if (!xfs_has_rtreflink(mp))
		return 0;

	return xfs_rtrefcountbt_max_size(mp,
			xfs_rtb_to_rtx(mp, mp->m_sb.sb_rgblocks));
}

/*
 * Convert on-disk form of btree root to in-memory form.
 */
STATIC void
xfs_rtrefcountbt_from_disk(
	struct xfs_inode		*ip,
	struct xfs_rtrefcount_root	*dblock,
	int				dblocklen,
	struct xfs_btree_block		*rblock)
{
	struct xfs_mount		*mp = ip->i_mount;
	struct xfs_refcount_key	*fkp;
	__be64				*fpp;
	struct xfs_refcount_key	*tkp;
	__be64				*tpp;
	struct xfs_refcount_rec	*frp;
	struct xfs_refcount_rec	*trp;
	unsigned int			numrecs;
	unsigned int			maxrecs;
	unsigned int			rblocklen;

	rblocklen = xfs_rtrefcount_broot_space(mp, dblock);

	xfs_btree_init_block(mp, rblock, &xfs_rtrefcountbt_ops, 0, 0,
			ip->i_ino);

	rblock->bb_level = dblock->bb_level;
	rblock->bb_numrecs = dblock->bb_numrecs;

	if (be16_to_cpu(rblock->bb_level) > 0) {
		maxrecs = xfs_rtrefcountbt_droot_maxrecs(dblocklen, false);
		fkp = xfs_rtrefcount_droot_key_addr(dblock, 1);
		tkp = xfs_rtrefcount_key_addr(rblock, 1);
		fpp = xfs_rtrefcount_droot_ptr_addr(dblock, 1, maxrecs);
		tpp = xfs_rtrefcount_broot_ptr_addr(mp, rblock, 1, rblocklen);
		numrecs = be16_to_cpu(dblock->bb_numrecs);
		memcpy(tkp, fkp, 2 * sizeof(*fkp) * numrecs);
		memcpy(tpp, fpp, sizeof(*fpp) * numrecs);
	} else {
		frp = xfs_rtrefcount_droot_rec_addr(dblock, 1);
		trp = xfs_rtrefcount_rec_addr(rblock, 1);
		numrecs = be16_to_cpu(dblock->bb_numrecs);
		memcpy(trp, frp, sizeof(*frp) * numrecs);
	}
}

/* Load a realtime reference count btree root in from disk. */
int
xfs_iformat_rtrefcount(
	struct xfs_inode	*ip,
	struct xfs_dinode	*dip)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	struct xfs_rtrefcount_root *dfp = XFS_DFORK_PTR(dip, XFS_DATA_FORK);
	unsigned int		numrecs;
	unsigned int		level;
	int			dsize;

	dsize = XFS_DFORK_SIZE(dip, mp, XFS_DATA_FORK);
	numrecs = be16_to_cpu(dfp->bb_numrecs);
	level = be16_to_cpu(dfp->bb_level);

	if (level > mp->m_rtrefc_maxlevels ||
	    xfs_rtrefcount_droot_space_calc(level, numrecs) > dsize)
		return -EFSCORRUPTED;

	xfs_iroot_alloc(ip, XFS_DATA_FORK,
			xfs_rtrefcount_broot_space_calc(mp, level, numrecs));
	xfs_rtrefcountbt_from_disk(ip, dfp, dsize, ifp->if_broot);
	return 0;
}

/*
 * Convert in-memory form of btree root to on-disk form.
 */
void
xfs_rtrefcountbt_to_disk(
	struct xfs_mount		*mp,
	struct xfs_btree_block		*rblock,
	int				rblocklen,
	struct xfs_rtrefcount_root	*dblock,
	int				dblocklen)
{
	struct xfs_refcount_key	*fkp;
	__be64				*fpp;
	struct xfs_refcount_key	*tkp;
	__be64				*tpp;
	struct xfs_refcount_rec	*frp;
	struct xfs_refcount_rec	*trp;
	unsigned int			maxrecs;
	unsigned int			numrecs;

	ASSERT(rblock->bb_magic == cpu_to_be32(XFS_RTREFC_CRC_MAGIC));
	ASSERT(uuid_equal(&rblock->bb_u.l.bb_uuid, &mp->m_sb.sb_meta_uuid));
	ASSERT(rblock->bb_u.l.bb_blkno == cpu_to_be64(XFS_BUF_DADDR_NULL));
	ASSERT(rblock->bb_u.l.bb_leftsib == cpu_to_be64(NULLFSBLOCK));
	ASSERT(rblock->bb_u.l.bb_rightsib == cpu_to_be64(NULLFSBLOCK));

	dblock->bb_level = rblock->bb_level;
	dblock->bb_numrecs = rblock->bb_numrecs;

	if (be16_to_cpu(rblock->bb_level) > 0) {
		maxrecs = xfs_rtrefcountbt_droot_maxrecs(dblocklen, false);
		fkp = xfs_rtrefcount_key_addr(rblock, 1);
		tkp = xfs_rtrefcount_droot_key_addr(dblock, 1);
		fpp = xfs_rtrefcount_broot_ptr_addr(mp, rblock, 1, rblocklen);
		tpp = xfs_rtrefcount_droot_ptr_addr(dblock, 1, maxrecs);
		numrecs = be16_to_cpu(rblock->bb_numrecs);
		memcpy(tkp, fkp, 2 * sizeof(*fkp) * numrecs);
		memcpy(tpp, fpp, sizeof(*fpp) * numrecs);
	} else {
		frp = xfs_rtrefcount_rec_addr(rblock, 1);
		trp = xfs_rtrefcount_droot_rec_addr(dblock, 1);
		numrecs = be16_to_cpu(rblock->bb_numrecs);
		memcpy(trp, frp, sizeof(*frp) * numrecs);
	}
}

/* Flush a realtime reference count btree root out to disk. */
void
xfs_iflush_rtrefcount(
	struct xfs_inode	*ip,
	struct xfs_dinode	*dip)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	struct xfs_rtrefcount_root *dfp = XFS_DFORK_PTR(dip, XFS_DATA_FORK);

	ASSERT(ifp->if_broot != NULL);
	ASSERT(ifp->if_broot_bytes > 0);
	ASSERT(xfs_rtrefcount_droot_space(ifp->if_broot) <=
			xfs_inode_fork_size(ip, XFS_DATA_FORK));
	xfs_rtrefcountbt_to_disk(ip->i_mount, ifp->if_broot,
			ifp->if_broot_bytes, dfp,
			XFS_DFORK_SIZE(dip, ip->i_mount, XFS_DATA_FORK));
}

/*
 * Create a realtime refcount btree inode.
 *
 * Regardless of the return value, the caller must clean up @upd.  If a new
 * inode is returned through *ipp, the caller must finish setting up the incore
 * inode and release it.
 */
int
xfs_rtrefcountbt_create(
	struct xfs_imeta_update	*upd,
	struct xfs_inode	**ipp)
{
	struct xfs_mount	*mp = upd->mp;
	struct xfs_ifork	*ifp;
	int			error;

	error = xfs_imeta_create(upd, S_IFREG, ipp);
	if (error)
		return error;

	ifp = xfs_ifork_ptr(upd->ip, XFS_DATA_FORK);
	ifp->if_format = XFS_DINODE_FMT_REFCOUNT;
	ASSERT(ifp->if_broot_bytes == 0);
	ASSERT(ifp->if_bytes == 0);

	/* Initialize the empty incore btree root. */
	xfs_iroot_alloc(upd->ip, XFS_DATA_FORK,
			xfs_rtrefcount_broot_space_calc(mp, 0, 0));
	xfs_btree_init_block(mp, ifp->if_broot, &xfs_rtrefcountbt_ops,
			0, 0, upd->ip->i_ino);
	xfs_trans_log_inode(upd->tp, upd->ip, XFS_ILOG_CORE | XFS_ILOG_DBROOT);
	return 0;
}
