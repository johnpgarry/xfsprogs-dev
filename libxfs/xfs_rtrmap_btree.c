// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2018-2024 Oracle.  All Rights Reserved.
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
#include "xfs_rmap.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_trace.h"
#include "xfs_cksum.h"
#include "xfs_rtgroup.h"
#include "xfs_bmap.h"
#include "xfs_imeta.h"

static struct kmem_cache	*xfs_rtrmapbt_cur_cache;

/*
 * Realtime Reverse Map btree.
 *
 * This is a btree used to track the owner(s) of a given extent in the realtime
 * device.  See the comments in xfs_rmap_btree.c for more information.
 *
 * This tree is basically the same as the regular rmap btree except that it
 * is rooted in an inode and does not live in free space.
 */

static struct xfs_btree_cur *
xfs_rtrmapbt_dup_cursor(
	struct xfs_btree_cur	*cur)
{
	struct xfs_btree_cur	*new;

	new = xfs_rtrmapbt_init_cursor(cur->bc_mp, cur->bc_tp, cur->bc_ino.rtg,
			cur->bc_ino.ip);

	return new;
}

STATIC int
xfs_rtrmapbt_get_minrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	if (level == cur->bc_nlevels - 1) {
		struct xfs_ifork	*ifp = xfs_btree_ifork_ptr(cur);

		return xfs_rtrmapbt_maxrecs(cur->bc_mp, ifp->if_broot_bytes,
				level == 0) / 2;
	}

	return cur->bc_mp->m_rtrmap_mnr[level != 0];
}

STATIC int
xfs_rtrmapbt_get_maxrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	if (level == cur->bc_nlevels - 1) {
		struct xfs_ifork	*ifp = xfs_btree_ifork_ptr(cur);

		return xfs_rtrmapbt_maxrecs(cur->bc_mp, ifp->if_broot_bytes,
				level == 0);
	}

	return cur->bc_mp->m_rtrmap_mxr[level != 0];
}

/*
 * Convert the ondisk record's offset field into the ondisk key's offset field.
 * Fork and bmbt are significant parts of the rmap record key, but written
 * status is merely a record attribute.
 */
static inline __be64 ondisk_rec_offset_to_key(const union xfs_btree_rec *rec)
{
	return rec->rmap.rm_offset & ~cpu_to_be64(XFS_RMAP_OFF_UNWRITTEN);
}

STATIC void
xfs_rtrmapbt_init_key_from_rec(
	union xfs_btree_key		*key,
	const union xfs_btree_rec	*rec)
{
	key->rmap.rm_startblock = rec->rmap.rm_startblock;
	key->rmap.rm_owner = rec->rmap.rm_owner;
	key->rmap.rm_offset = ondisk_rec_offset_to_key(rec);
}

STATIC void
xfs_rtrmapbt_init_high_key_from_rec(
	union xfs_btree_key		*key,
	const union xfs_btree_rec	*rec)
{
	uint64_t			off;
	int				adj;

	adj = be32_to_cpu(rec->rmap.rm_blockcount) - 1;

	key->rmap.rm_startblock = rec->rmap.rm_startblock;
	be32_add_cpu(&key->rmap.rm_startblock, adj);
	key->rmap.rm_owner = rec->rmap.rm_owner;
	key->rmap.rm_offset = ondisk_rec_offset_to_key(rec);
	if (XFS_RMAP_NON_INODE_OWNER(be64_to_cpu(rec->rmap.rm_owner)) ||
	    XFS_RMAP_IS_BMBT_BLOCK(be64_to_cpu(rec->rmap.rm_offset)))
		return;
	off = be64_to_cpu(key->rmap.rm_offset);
	off = (XFS_RMAP_OFF(off) + adj) | (off & ~XFS_RMAP_OFF_MASK);
	key->rmap.rm_offset = cpu_to_be64(off);
}

STATIC void
xfs_rtrmapbt_init_rec_from_cur(
	struct xfs_btree_cur	*cur,
	union xfs_btree_rec	*rec)
{
	rec->rmap.rm_startblock = cpu_to_be32(cur->bc_rec.r.rm_startblock);
	rec->rmap.rm_blockcount = cpu_to_be32(cur->bc_rec.r.rm_blockcount);
	rec->rmap.rm_owner = cpu_to_be64(cur->bc_rec.r.rm_owner);
	rec->rmap.rm_offset = cpu_to_be64(
			xfs_rmap_irec_offset_pack(&cur->bc_rec.r));
}

STATIC void
xfs_rtrmapbt_init_ptr_from_cur(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr)
{
	ptr->l = 0;
}

/*
 * Mask the appropriate parts of the ondisk key field for a key comparison.
 * Fork and bmbt are significant parts of the rmap record key, but written
 * status is merely a record attribute.
 */
static inline uint64_t offset_keymask(uint64_t offset)
{
	return offset & ~XFS_RMAP_OFF_UNWRITTEN;
}

STATIC int64_t
xfs_rtrmapbt_key_diff(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key)
{
	struct xfs_rmap_irec		*rec = &cur->bc_rec.r;
	const struct xfs_rmap_key	*kp = &key->rmap;
	__u64				x, y;
	int64_t				d;

	d = (int64_t)be32_to_cpu(kp->rm_startblock) - rec->rm_startblock;
	if (d)
		return d;

	x = be64_to_cpu(kp->rm_owner);
	y = rec->rm_owner;
	if (x > y)
		return 1;
	else if (y > x)
		return -1;

	x = offset_keymask(be64_to_cpu(kp->rm_offset));
	y = offset_keymask(xfs_rmap_irec_offset_pack(rec));
	if (x > y)
		return 1;
	else if (y > x)
		return -1;
	return 0;
}

STATIC int64_t
xfs_rtrmapbt_diff_two_keys(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*k1,
	const union xfs_btree_key	*k2,
	const union xfs_btree_key	*mask)
{
	const struct xfs_rmap_key	*kp1 = &k1->rmap;
	const struct xfs_rmap_key	*kp2 = &k2->rmap;
	int64_t				d;
	__u64				x, y;

	/* Doesn't make sense to mask off the physical space part */
	ASSERT(!mask || mask->rmap.rm_startblock);

	d = (int64_t)be32_to_cpu(kp1->rm_startblock) -
		     be32_to_cpu(kp2->rm_startblock);
	if (d)
		return d;

	if (!mask || mask->rmap.rm_owner) {
		x = be64_to_cpu(kp1->rm_owner);
		y = be64_to_cpu(kp2->rm_owner);
		if (x > y)
			return 1;
		else if (y > x)
			return -1;
	}

	if (!mask || mask->rmap.rm_offset) {
		/* Doesn't make sense to allow offset but not owner */
		ASSERT(!mask || mask->rmap.rm_owner);

		x = offset_keymask(be64_to_cpu(kp1->rm_offset));
		y = offset_keymask(be64_to_cpu(kp2->rm_offset));
		if (x > y)
			return 1;
		else if (y > x)
			return -1;
	}

	return 0;
}

static xfs_failaddr_t
xfs_rtrmapbt_verify(
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = bp->b_target->bt_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	xfs_failaddr_t		fa;
	int			level;

	if (!xfs_verify_magic(bp, block->bb_magic))
		return __this_address;

	if (!xfs_has_rmapbt(mp))
		return __this_address;
	fa = xfs_btree_lblock_v5hdr_verify(bp, XFS_RMAP_OWN_UNKNOWN);
	if (fa)
		return fa;
	level = be16_to_cpu(block->bb_level);
	if (level > mp->m_rtrmap_maxlevels)
		return __this_address;

	return xfs_btree_lblock_verify(bp, mp->m_rtrmap_mxr[level != 0]);
}

static void
xfs_rtrmapbt_read_verify(
	struct xfs_buf	*bp)
{
	xfs_failaddr_t	fa;

	if (!xfs_btree_lblock_verify_crc(bp))
		xfs_verifier_error(bp, -EFSBADCRC, __this_address);
	else {
		fa = xfs_rtrmapbt_verify(bp);
		if (fa)
			xfs_verifier_error(bp, -EFSCORRUPTED, fa);
	}

	if (bp->b_error)
		trace_xfs_btree_corrupt(bp, _RET_IP_);
}

static void
xfs_rtrmapbt_write_verify(
	struct xfs_buf	*bp)
{
	xfs_failaddr_t	fa;

	fa = xfs_rtrmapbt_verify(bp);
	if (fa) {
		trace_xfs_btree_corrupt(bp, _RET_IP_);
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
		return;
	}
	xfs_btree_lblock_calc_crc(bp);

}

const struct xfs_buf_ops xfs_rtrmapbt_buf_ops = {
	.name			= "xfs_rtrmapbt",
	.magic			= { 0, cpu_to_be32(XFS_RTRMAP_CRC_MAGIC) },
	.verify_read		= xfs_rtrmapbt_read_verify,
	.verify_write		= xfs_rtrmapbt_write_verify,
	.verify_struct		= xfs_rtrmapbt_verify,
};

STATIC int
xfs_rtrmapbt_keys_inorder(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*k1,
	const union xfs_btree_key	*k2)
{
	uint32_t			x;
	uint32_t			y;
	uint64_t			a;
	uint64_t			b;

	x = be32_to_cpu(k1->rmap.rm_startblock);
	y = be32_to_cpu(k2->rmap.rm_startblock);
	if (x < y)
		return 1;
	else if (x > y)
		return 0;
	a = be64_to_cpu(k1->rmap.rm_owner);
	b = be64_to_cpu(k2->rmap.rm_owner);
	if (a < b)
		return 1;
	else if (a > b)
		return 0;
	a = offset_keymask(be64_to_cpu(k1->rmap.rm_offset));
	b = offset_keymask(be64_to_cpu(k2->rmap.rm_offset));
	if (a <= b)
		return 1;
	return 0;
}

STATIC int
xfs_rtrmapbt_recs_inorder(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_rec	*r1,
	const union xfs_btree_rec	*r2)
{
	uint32_t			x;
	uint32_t			y;
	uint64_t			a;
	uint64_t			b;

	x = be32_to_cpu(r1->rmap.rm_startblock);
	y = be32_to_cpu(r2->rmap.rm_startblock);
	if (x < y)
		return 1;
	else if (x > y)
		return 0;
	a = be64_to_cpu(r1->rmap.rm_owner);
	b = be64_to_cpu(r2->rmap.rm_owner);
	if (a < b)
		return 1;
	else if (a > b)
		return 0;
	a = offset_keymask(be64_to_cpu(r1->rmap.rm_offset));
	b = offset_keymask(be64_to_cpu(r2->rmap.rm_offset));
	if (a <= b)
		return 1;
	return 0;
}

STATIC enum xbtree_key_contig
xfs_rtrmapbt_keys_contiguous(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2,
	const union xfs_btree_key	*mask)
{
	ASSERT(!mask || mask->rmap.rm_startblock);

	/*
	 * We only support checking contiguity of the physical space component.
	 * If any callers ever need more specificity than that, they'll have to
	 * implement it here.
	 */
	ASSERT(!mask || (!mask->rmap.rm_owner && !mask->rmap.rm_offset));

	return xbtree_key_contig(be32_to_cpu(key1->rmap.rm_startblock),
				 be32_to_cpu(key2->rmap.rm_startblock));
}

const struct xfs_btree_ops xfs_rtrmapbt_ops = {
	.rec_len		= sizeof(struct xfs_rmap_rec),
	.key_len		= 2 * sizeof(struct xfs_rmap_key),
	.geom_flags		= XFS_BTGEO_LONG_PTRS | XFS_BTGEO_ROOT_IN_INODE |
				  XFS_BTGEO_CRC_BLOCKS | XFS_BTGEO_OVERLAPPING |
				  XFS_BTGEO_IROOT_RECORDS,
	.lru_refs		= XFS_RMAP_BTREE_REF,

	.dup_cursor		= xfs_rtrmapbt_dup_cursor,
	.alloc_block		= xfs_btree_alloc_imeta_block,
	.free_block		= xfs_btree_free_imeta_block,
	.get_minrecs		= xfs_rtrmapbt_get_minrecs,
	.get_maxrecs		= xfs_rtrmapbt_get_maxrecs,
	.init_key_from_rec	= xfs_rtrmapbt_init_key_from_rec,
	.init_high_key_from_rec	= xfs_rtrmapbt_init_high_key_from_rec,
	.init_rec_from_cur	= xfs_rtrmapbt_init_rec_from_cur,
	.init_ptr_from_cur	= xfs_rtrmapbt_init_ptr_from_cur,
	.key_diff		= xfs_rtrmapbt_key_diff,
	.buf_ops		= &xfs_rtrmapbt_buf_ops,
	.diff_two_keys		= xfs_rtrmapbt_diff_two_keys,
	.keys_inorder		= xfs_rtrmapbt_keys_inorder,
	.recs_inorder		= xfs_rtrmapbt_recs_inorder,
	.keys_contiguous	= xfs_rtrmapbt_keys_contiguous,
};

/* Initialize a new rt rmap btree cursor. */
static struct xfs_btree_cur *
xfs_rtrmapbt_init_common(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_rtgroup	*rtg,
	struct xfs_inode	*ip)
{
	struct xfs_btree_cur	*cur;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_SHARED | XFS_ILOCK_EXCL));

	cur = xfs_btree_alloc_cursor(mp, tp, XFS_BTNUM_RTRMAP,
			&xfs_rtrmapbt_ops, mp->m_rtrmap_maxlevels,
			xfs_rtrmapbt_cur_cache);
	cur->bc_statoff = XFS_STATS_CALC_INDEX(xs_rmap_2);

	cur->bc_ino.ip = ip;
	cur->bc_ino.allocated = 0;

	cur->bc_ino.rtg = xfs_rtgroup_hold(rtg);
	return cur;
}

/* Allocate a new rt rmap btree cursor. */
struct xfs_btree_cur *
xfs_rtrmapbt_init_cursor(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_rtgroup	*rtg,
	struct xfs_inode	*ip)
{
	struct xfs_btree_cur	*cur;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);

	cur = xfs_rtrmapbt_init_common(mp, tp, rtg, ip);
	cur->bc_nlevels = be16_to_cpu(ifp->if_broot->bb_level) + 1;
	cur->bc_ino.forksize = xfs_inode_fork_size(ip, XFS_DATA_FORK);
	cur->bc_ino.whichfork = XFS_DATA_FORK;
	return cur;
}

/* Create a new rt reverse mapping btree cursor with a fake root for staging. */
struct xfs_btree_cur *
xfs_rtrmapbt_stage_cursor(
	struct xfs_mount	*mp,
	struct xfs_rtgroup	*rtg,
	struct xfs_inode	*ip,
	struct xbtree_ifakeroot	*ifake)
{
	struct xfs_btree_cur	*cur;

	cur = xfs_rtrmapbt_init_common(mp, NULL, rtg, ip);
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
xfs_rtrmapbt_commit_staged_btree(
	struct xfs_btree_cur	*cur,
	struct xfs_trans	*tp)
{
	struct xbtree_ifakeroot	*ifake = cur->bc_ino.ifake;
	struct xfs_ifork	*ifp;
	int			flags = XFS_ILOG_CORE | XFS_ILOG_DBROOT;

	ASSERT(xfs_btree_is_staging(cur));
	ASSERT(ifake->if_fork->if_format == XFS_DINODE_FMT_RMAP);

	/*
	 * Free any resources hanging off the real fork, then shallow-copy the
	 * staging fork's contents into the real fork to transfer everything
	 * we just built.
	 */
	ifp = xfs_ifork_ptr(cur->bc_ino.ip, XFS_DATA_FORK);
	xfs_idestroy_fork(ifp);
	memcpy(ifp, ifake->if_fork, sizeof(struct xfs_ifork));

	xfs_trans_log_inode(tp, cur->bc_ino.ip, flags);
	xfs_btree_commit_ifakeroot(cur, tp, XFS_DATA_FORK, &xfs_rtrmapbt_ops);
}

/* Calculate number of records in a rt reverse mapping btree block. */
static inline unsigned int
xfs_rtrmapbt_block_maxrecs(
	unsigned int		blocklen,
	bool			leaf)
{
	if (leaf)
		return blocklen / sizeof(struct xfs_rmap_rec);
	return blocklen /
		(2 * sizeof(struct xfs_rmap_key) + sizeof(xfs_rtrmap_ptr_t));
}

/*
 * Calculate number of records in an rt reverse mapping btree block.
 */
unsigned int
xfs_rtrmapbt_maxrecs(
	struct xfs_mount	*mp,
	unsigned int		blocklen,
	bool			leaf)
{
	blocklen -= XFS_RTRMAP_BLOCK_LEN;
	return xfs_rtrmapbt_block_maxrecs(blocklen, leaf);
}

/* Compute the max possible height for realtime reverse mapping btrees. */
unsigned int
xfs_rtrmapbt_maxlevels_ondisk(void)
{
	unsigned int		minrecs[2];
	unsigned int		blocklen;

	blocklen = XFS_MIN_CRC_BLOCKSIZE - XFS_BTREE_LBLOCK_CRC_LEN;

	minrecs[0] = xfs_rtrmapbt_block_maxrecs(blocklen, true) / 2;
	minrecs[1] = xfs_rtrmapbt_block_maxrecs(blocklen, false) / 2;

	/* We need at most one record for every block in an rt group. */
	return xfs_btree_compute_maxlevels(minrecs, XFS_MAX_RGBLOCKS);
}

int __init
xfs_rtrmapbt_init_cur_cache(void)
{
	xfs_rtrmapbt_cur_cache = kmem_cache_create("xfs_rtrmapbt_cur",
			xfs_btree_cur_sizeof(xfs_rtrmapbt_maxlevels_ondisk()),
			0, 0, NULL);

	if (!xfs_rtrmapbt_cur_cache)
		return -ENOMEM;
	return 0;
}

void
xfs_rtrmapbt_destroy_cur_cache(void)
{
	kmem_cache_destroy(xfs_rtrmapbt_cur_cache);
	xfs_rtrmapbt_cur_cache = NULL;
}

/* Compute the maximum height of an rt reverse mapping btree. */
void
xfs_rtrmapbt_compute_maxlevels(
	struct xfs_mount	*mp)
{
	unsigned int		d_maxlevels, r_maxlevels;

	if (!xfs_has_rtrmapbt(mp)) {
		mp->m_rtrmap_maxlevels = 0;
		return;
	}

	/*
	 * The realtime rmapbt lives on the data device, which means that its
	 * maximum height is constrained by the size of the data device and
	 * the height required to store one rmap record for each block in an
	 * rt group.
	 */
	d_maxlevels = xfs_btree_space_to_height(mp->m_rtrmap_mnr,
				mp->m_sb.sb_dblocks);
	r_maxlevels = xfs_btree_compute_maxlevels(mp->m_rtrmap_mnr,
				mp->m_sb.sb_rgblocks);

	/* Add one level to handle the inode root level. */
	mp->m_rtrmap_maxlevels = min(d_maxlevels, r_maxlevels) + 1;
}

#define XFS_RTRMAP_NAMELEN		17

/* Create the metadata directory path for an rtrmap btree inode. */
int
xfs_rtrmapbt_create_path(
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

	fname = kmalloc(XFS_RTRMAP_NAMELEN, GFP_KERNEL);
	if (!fname) {
		xfs_imeta_free_path(path);
		return -ENOMEM;
	}

	snprintf(fname, XFS_RTRMAP_NAMELEN, "%u.rmap", rgno);
	path->im_path[0] = "realtime";
	path->im_path[1] = fname;
	path->im_dynamicmask = 0x2;
	*pathp = path;
	return 0;
}
