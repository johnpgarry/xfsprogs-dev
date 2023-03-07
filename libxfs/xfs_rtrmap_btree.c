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
#include "xfs_health.h"
#include "xfile.h"
#include "xfbtree.h"
#include "xfs_btree_mem.h"

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

/* Calculate number of records in the ondisk realtime rmap btree inode root. */
unsigned int
xfs_rtrmapbt_droot_maxrecs(
	unsigned int		blocklen,
	bool			leaf)
{
	blocklen -= sizeof(struct xfs_rtrmap_root);

	if (leaf)
		return blocklen / sizeof(struct xfs_rmap_rec);
	return blocklen / (2 * sizeof(struct xfs_rmap_key) +
			sizeof(xfs_rtrmap_ptr_t));
}

/*
 * Get the maximum records we could store in the on-disk format.
 *
 * For non-root nodes this is equivalent to xfs_rtrmapbt_get_maxrecs, but
 * for the root node this checks the available space in the dinode fork
 * so that we can resize the in-memory buffer to match it.  After a
 * resize to the maximum size this function returns the same value
 * as xfs_rtrmapbt_get_maxrecs for the root node, too.
 */
STATIC int
xfs_rtrmapbt_get_dmaxrecs(
	struct xfs_btree_cur	*cur,
	int			level)
{
	if (level != cur->bc_nlevels - 1)
		return cur->bc_mp->m_rtrmap_mxr[level != 0];
	return xfs_rtrmapbt_droot_maxrecs(cur->bc_ino.forksize, level == 0);
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

/* Move the rtrmap btree root from one incore buffer to another. */
static void
xfs_rtrmapbt_broot_move(
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

	ASSERT(xfs_rtrmap_droot_space(src_broot) <=
			xfs_inode_fork_size(ip, whichfork));

	/*
	 * We always have to move the pointers because they are not butted
	 * against the btree block header.
	 */
	if (numrecs && level > 0) {
		sptr = xfs_rtrmap_broot_ptr_addr(mp, src_broot, 1, src_bytes);
		dptr = xfs_rtrmap_broot_ptr_addr(mp, dst_broot, 1, dst_bytes);
		memmove(dptr, sptr, numrecs * sizeof(xfs_fsblock_t));
	}

	if (src_broot == dst_broot)
		return;

	/*
	 * If the root is being totally relocated, we have to migrate the block
	 * header and the keys/records that come after it.
	 */
	memcpy(dst_broot, src_broot, XFS_RTRMAP_BLOCK_LEN);

	if (!numrecs)
		return;

	if (level == 0) {
		sptr = xfs_rtrmap_rec_addr(src_broot, 1);
		dptr = xfs_rtrmap_rec_addr(dst_broot, 1);
		memcpy(dptr, sptr, numrecs * sizeof(struct xfs_rmap_rec));
	} else {
		sptr = xfs_rtrmap_key_addr(src_broot, 1);
		dptr = xfs_rtrmap_key_addr(dst_broot, 1);
		memcpy(dptr, sptr, numrecs * 2 * sizeof(struct xfs_rmap_key));
	}
}

static const struct xfs_ifork_broot_ops xfs_rtrmapbt_iroot_ops = {
	.maxrecs		= xfs_rtrmapbt_maxrecs,
	.size			= xfs_rtrmap_broot_space_calc,
	.move			= xfs_rtrmapbt_broot_move,
};

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
	.get_dmaxrecs		= xfs_rtrmapbt_get_dmaxrecs,
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
	.iroot_ops		= &xfs_rtrmapbt_iroot_ops,
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

#ifdef CONFIG_XFS_BTREE_IN_XFILE
/*
 * Validate an in-memory realtime rmap btree block.  Callers are allowed to
 * generate an in-memory btree even if the ondisk feature is not enabled.
 */
static xfs_failaddr_t
xfs_rtrmapbt_mem_verify(
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = bp->b_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	xfs_failaddr_t		fa;
	unsigned int		level;

	if (!xfs_verify_magic(bp, block->bb_magic))
		return __this_address;

	fa = xfs_btree_lblock_v5hdr_verify(bp, XFS_RMAP_OWN_UNKNOWN);
	if (fa)
		return fa;

	level = be16_to_cpu(block->bb_level);
	if (xfs_has_rmapbt(mp)) {
		if (level >= mp->m_rtrmap_maxlevels)
			return __this_address;
	} else {
		if (level >= xfs_rtrmapbt_maxlevels_ondisk())
			return __this_address;
	}

	return xfbtree_lblock_verify(bp,
			xfs_rtrmapbt_maxrecs(mp, xfo_to_b(1), level == 0));
}

static void
xfs_rtrmapbt_mem_rw_verify(
	struct xfs_buf	*bp)
{
	xfs_failaddr_t	fa = xfs_rtrmapbt_mem_verify(bp);

	if (fa)
		xfs_verifier_error(bp, -EFSCORRUPTED, fa);
}

/* skip crc checks on in-memory btrees to save time */
static const struct xfs_buf_ops xfs_rtrmapbt_mem_buf_ops = {
	.name			= "xfs_rtrmapbt_mem",
	.magic			= { 0, cpu_to_be32(XFS_RTRMAP_CRC_MAGIC) },
	.verify_read		= xfs_rtrmapbt_mem_rw_verify,
	.verify_write		= xfs_rtrmapbt_mem_rw_verify,
	.verify_struct		= xfs_rtrmapbt_mem_verify,
};

static const struct xfs_btree_ops xfs_rtrmapbt_mem_ops = {
	.rec_len		= sizeof(struct xfs_rmap_rec),
	.key_len		= 2 * sizeof(struct xfs_rmap_key),
	.geom_flags		= XFS_BTGEO_CRC_BLOCKS | XFS_BTGEO_OVERLAPPING |
				  XFS_BTGEO_LONG_PTRS | XFS_BTGEO_IN_XFILE,
	.lru_refs		= XFS_RMAP_BTREE_REF,

	.dup_cursor		= xfbtree_dup_cursor,
	.set_root		= xfbtree_set_root,
	.alloc_block		= xfbtree_alloc_block,
	.free_block		= xfbtree_free_block,
	.get_minrecs		= xfbtree_get_minrecs,
	.get_maxrecs		= xfbtree_get_maxrecs,
	.init_key_from_rec	= xfs_rtrmapbt_init_key_from_rec,
	.init_high_key_from_rec	= xfs_rtrmapbt_init_high_key_from_rec,
	.init_rec_from_cur	= xfs_rtrmapbt_init_rec_from_cur,
	.init_ptr_from_cur	= xfbtree_init_ptr_from_cur,
	.key_diff		= xfs_rtrmapbt_key_diff,
	.buf_ops		= &xfs_rtrmapbt_mem_buf_ops,
	.diff_two_keys		= xfs_rtrmapbt_diff_two_keys,
	.keys_inorder		= xfs_rtrmapbt_keys_inorder,
	.recs_inorder		= xfs_rtrmapbt_recs_inorder,
	.keys_contiguous	= xfs_rtrmapbt_keys_contiguous,
};

/* Create a cursor for an in-memory btree. */
struct xfs_btree_cur *
xfs_rtrmapbt_mem_cursor(
	struct xfs_rtgroup	*rtg,
	struct xfs_trans	*tp,
	struct xfbtree		*xfbt)
{
	struct xfs_btree_cur	*cur;
	struct xfs_mount	*mp = rtg->rtg_mount;

	/* Overlapping btree; 2 keys per pointer. */
	cur = xfs_btree_alloc_cursor(mp, tp, XFS_BTNUM_RTRMAP,
			&xfs_rtrmapbt_mem_ops, mp->m_rtrmap_maxlevels,
			xfs_rtrmapbt_cur_cache);
	cur->bc_statoff = XFS_STATS_CALC_INDEX(xs_rmap_2);
	cur->bc_mem.xfbtree = xfbt;
	cur->bc_nlevels = xfbt->nlevels;

	cur->bc_mem.rtg = xfs_rtgroup_hold(rtg);
	return cur;
}

/* Create an in-memory realtime rmap btree. */
int
xfs_rtrmapbt_mem_init(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno,
	struct xfs_buftarg	*target,
	struct xfbtree		*xfbt)
{
	xfbt->target = target;
	xfbt->owner = rgno;
	return xfbtree_init(mp, xfbt, &xfs_rtrmapbt_mem_ops);
}
#endif /* CONFIG_XFS_BTREE_IN_XFILE */

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

/* Calculate the rtrmap btree size for some records. */
unsigned long long
xfs_rtrmapbt_calc_size(
	struct xfs_mount	*mp,
	unsigned long long	len)
{
	return xfs_btree_calc_size(mp->m_rtrmap_mnr, len);
}

/*
 * Calculate the maximum rmap btree size.
 */
static unsigned long long
xfs_rtrmapbt_max_size(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtblocks)
{
	/* Bail out if we're uninitialized, which can happen in mkfs. */
	if (mp->m_rtrmap_mxr[0] == 0)
		return 0;

	return xfs_rtrmapbt_calc_size(mp, rtblocks);
}

/*
 * Figure out how many blocks to reserve and how many are used by this btree.
 */
xfs_filblks_t
xfs_rtrmapbt_calc_reserves(
	struct xfs_mount	*mp)
{
	if (!xfs_has_rtrmapbt(mp))
		return 0;

	/* 1/64th (~1.5%) of the space, and enough for 1 record per block. */
	return max_t(xfs_filblks_t, mp->m_sb.sb_rgblocks >> 6,
			xfs_rtrmapbt_max_size(mp, mp->m_sb.sb_rgblocks));
}

/* Convert on-disk form of btree root to in-memory form. */
STATIC void
xfs_rtrmapbt_from_disk(
	struct xfs_inode	*ip,
	struct xfs_rtrmap_root	*dblock,
	unsigned int		dblocklen,
	struct xfs_btree_block	*rblock)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_rmap_key	*fkp;
	__be64			*fpp;
	struct xfs_rmap_key	*tkp;
	__be64			*tpp;
	struct xfs_rmap_rec	*frp;
	struct xfs_rmap_rec	*trp;
	unsigned int		rblocklen = xfs_rtrmap_broot_space(mp, dblock);
	unsigned int		numrecs;
	unsigned int		maxrecs;

	xfs_btree_init_block(mp, rblock, &xfs_rtrmapbt_ops, 0, 0, ip->i_ino);

	rblock->bb_level = dblock->bb_level;
	rblock->bb_numrecs = dblock->bb_numrecs;
	numrecs = be16_to_cpu(dblock->bb_numrecs);

	if (be16_to_cpu(rblock->bb_level) > 0) {
		maxrecs = xfs_rtrmapbt_droot_maxrecs(dblocklen, false);
		fkp = xfs_rtrmap_droot_key_addr(dblock, 1);
		tkp = xfs_rtrmap_key_addr(rblock, 1);
		fpp = xfs_rtrmap_droot_ptr_addr(dblock, 1, maxrecs);
		tpp = xfs_rtrmap_broot_ptr_addr(mp, rblock, 1, rblocklen);
		memcpy(tkp, fkp, 2 * sizeof(*fkp) * numrecs);
		memcpy(tpp, fpp, sizeof(*fpp) * numrecs);
	} else {
		frp = xfs_rtrmap_droot_rec_addr(dblock, 1);
		trp = xfs_rtrmap_rec_addr(rblock, 1);
		memcpy(trp, frp, sizeof(*frp) * numrecs);
	}
}

/* Load a realtime reverse mapping btree root in from disk. */
int
xfs_iformat_rtrmap(
	struct xfs_inode	*ip,
	struct xfs_dinode	*dip)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	struct xfs_rtrmap_root	*dfp = XFS_DFORK_PTR(dip, XFS_DATA_FORK);
	unsigned int		numrecs;
	unsigned int		level;
	int			dsize;

	dsize = XFS_DFORK_SIZE(dip, mp, XFS_DATA_FORK);
	numrecs = be16_to_cpu(dfp->bb_numrecs);
	level = be16_to_cpu(dfp->bb_level);

	if (level > mp->m_rtrmap_maxlevels ||
	    xfs_rtrmap_droot_space_calc(level, numrecs) > dsize) {
		xfs_inode_mark_sick(ip, XFS_SICK_INO_CORE);
		return -EFSCORRUPTED;
	}

	xfs_iroot_alloc(ip, XFS_DATA_FORK,
			xfs_rtrmap_broot_space_calc(mp, level, numrecs));
	xfs_rtrmapbt_from_disk(ip, dfp, dsize, ifp->if_broot);
	return 0;
}

/* Convert in-memory form of btree root to on-disk form. */
void
xfs_rtrmapbt_to_disk(
	struct xfs_mount	*mp,
	struct xfs_btree_block	*rblock,
	unsigned int		rblocklen,
	struct xfs_rtrmap_root	*dblock,
	unsigned int		dblocklen)
{
	struct xfs_rmap_key	*fkp;
	__be64			*fpp;
	struct xfs_rmap_key	*tkp;
	__be64			*tpp;
	struct xfs_rmap_rec	*frp;
	struct xfs_rmap_rec	*trp;
	unsigned int		numrecs;
	unsigned int		maxrecs;

	ASSERT(rblock->bb_magic == cpu_to_be32(XFS_RTRMAP_CRC_MAGIC));
	ASSERT(uuid_equal(&rblock->bb_u.l.bb_uuid, &mp->m_sb.sb_meta_uuid));
	ASSERT(rblock->bb_u.l.bb_blkno == cpu_to_be64(XFS_BUF_DADDR_NULL));
	ASSERT(rblock->bb_u.l.bb_leftsib == cpu_to_be64(NULLFSBLOCK));
	ASSERT(rblock->bb_u.l.bb_rightsib == cpu_to_be64(NULLFSBLOCK));

	dblock->bb_level = rblock->bb_level;
	dblock->bb_numrecs = rblock->bb_numrecs;
	numrecs = be16_to_cpu(rblock->bb_numrecs);

	if (be16_to_cpu(rblock->bb_level) > 0) {
		maxrecs = xfs_rtrmapbt_droot_maxrecs(dblocklen, false);
		fkp = xfs_rtrmap_key_addr(rblock, 1);
		tkp = xfs_rtrmap_droot_key_addr(dblock, 1);
		fpp = xfs_rtrmap_broot_ptr_addr(mp, rblock, 1, rblocklen);
		tpp = xfs_rtrmap_droot_ptr_addr(dblock, 1, maxrecs);
		memcpy(tkp, fkp, 2 * sizeof(*fkp) * numrecs);
		memcpy(tpp, fpp, sizeof(*fpp) * numrecs);
	} else {
		frp = xfs_rtrmap_rec_addr(rblock, 1);
		trp = xfs_rtrmap_droot_rec_addr(dblock, 1);
		memcpy(trp, frp, sizeof(*frp) * numrecs);
	}
}

/* Flush a realtime reverse mapping btree root out to disk. */
void
xfs_iflush_rtrmap(
	struct xfs_inode	*ip,
	struct xfs_dinode	*dip)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	struct xfs_rtrmap_root	*dfp = XFS_DFORK_PTR(dip, XFS_DATA_FORK);

	ASSERT(ifp->if_broot != NULL);
	ASSERT(ifp->if_broot_bytes > 0);
	ASSERT(xfs_rtrmap_droot_space(ifp->if_broot) <=
			xfs_inode_fork_size(ip, XFS_DATA_FORK));
	xfs_rtrmapbt_to_disk(ip->i_mount, ifp->if_broot, ifp->if_broot_bytes,
			dfp, XFS_DFORK_SIZE(dip, ip->i_mount, XFS_DATA_FORK));
}

/*
 * Create a realtime rmap btree inode.
 *
 * Regardless of the return value, the caller must clean up @upd.  If a new
 * inode is returned through @*ipp, the caller must finish setting up the incore
 * inode and release it.
 */
int
xfs_rtrmapbt_create(
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
	ifp->if_format = XFS_DINODE_FMT_RMAP;
	ASSERT(ifp->if_broot_bytes == 0);
	ASSERT(ifp->if_bytes == 0);

	/* Initialize the empty incore btree root. */
	xfs_iroot_alloc(upd->ip, XFS_DATA_FORK,
			xfs_rtrmap_broot_space_calc(mp, 0, 0));
	xfs_btree_init_block(mp, ifp->if_broot, &xfs_rtrmapbt_ops, 0, 0,
			upd->ip->i_ino);
	xfs_trans_log_inode(upd->tp, upd->ip, XFS_ILOG_CORE | XFS_ILOG_DBROOT);
	return 0;
}
