// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_swapext.h"
#include "xfs_trace.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_quota_defs.h"
#include "xfs_health.h"
#include "defer_item.h"
#include "xfs_errortag.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr_leaf.h"
#include "xfs_attr.h"
#include "xfs_dir2_priv.h"
#include "xfs_dir2.h"
#include "xfs_symlink_remote.h"
#include "xfs_rtbitmap.h"

struct kmem_cache	*xfs_swapext_intent_cache;

/* bmbt mappings adjacent to a pair of records. */
struct xfs_swapext_adjacent {
	struct xfs_bmbt_irec		left1;
	struct xfs_bmbt_irec		right1;
	struct xfs_bmbt_irec		left2;
	struct xfs_bmbt_irec		right2;
};

#define ADJACENT_INIT { \
	.left1  = { .br_startblock = HOLESTARTBLOCK }, \
	.right1 = { .br_startblock = HOLESTARTBLOCK }, \
	.left2  = { .br_startblock = HOLESTARTBLOCK }, \
	.right2 = { .br_startblock = HOLESTARTBLOCK }, \
}

/* Information to help us reset reflink flag / CoW fork state after a swap. */

/* Previous state of the two inodes' reflink flags. */
#define XFS_REFLINK_STATE_IP1		(1U << 0)
#define XFS_REFLINK_STATE_IP2		(1U << 1)

/*
 * If the reflink flag is set on either inode, make sure it has an incore CoW
 * fork, since all reflink inodes must have them.  If there's a CoW fork and it
 * has extents in it, make sure the inodes are tagged appropriately so that
 * speculative preallocations can be GC'd if we run low of space.
 */
static inline void
xfs_swapext_ensure_cowfork(
	struct xfs_inode	*ip)
{
	struct xfs_ifork	*cfork;

	if (xfs_is_reflink_inode(ip))
		xfs_ifork_init_cow(ip);

	cfork = xfs_ifork_ptr(ip, XFS_COW_FORK);
	if (!cfork)
		return;
	if (cfork->if_bytes > 0)
		xfs_inode_set_cowblocks_tag(ip);
	else
		xfs_inode_clear_cowblocks_tag(ip);
}

/*
 * Adjust the on-disk inode size upwards if needed so that we never map extents
 * into the file past EOF.  This is crucial so that log recovery won't get
 * confused by the sudden appearance of post-eof extents.
 */
STATIC void
xfs_swapext_update_size(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	struct xfs_bmbt_irec	*imap,
	xfs_fsize_t		new_isize)
{
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_fsize_t		len;

	if (new_isize < 0)
		return;

	len = min(XFS_FSB_TO_B(mp, imap->br_startoff + imap->br_blockcount),
		  new_isize);

	if (len <= ip->i_disk_size)
		return;

	trace_xfs_swapext_update_inode_size(ip, len);

	ip->i_disk_size = len;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}

static inline bool
sxi_has_more_swap_work(const struct xfs_swapext_intent *sxi)
{
	return sxi->sxi_blockcount > 0;
}

static inline bool
sxi_has_postop_work(const struct xfs_swapext_intent *sxi)
{
	return sxi->sxi_flags & (XFS_SWAP_EXT_CLEAR_INO1_REFLINK |
				 XFS_SWAP_EXT_CLEAR_INO2_REFLINK |
				 XFS_SWAP_EXT_CVT_INO2_SF);
}

static inline void
sxi_advance(
	struct xfs_swapext_intent	*sxi,
	const struct xfs_bmbt_irec	*irec)
{
	sxi->sxi_startoff1 += irec->br_blockcount;
	sxi->sxi_startoff2 += irec->br_blockcount;
	sxi->sxi_blockcount -= irec->br_blockcount;
}

#ifdef DEBUG
/*
 * If we're going to do a BUI-only extent swap, ensure that all mappings are
 * aligned to the realtime extent size.
 */
static inline int
xfs_swapext_check_rt_extents(
	struct xfs_mount		*mp,
	const struct xfs_swapext_req	*req)
{
	struct xfs_bmbt_irec		irec1, irec2;
	xfs_fileoff_t			startoff1 = req->startoff1;
	xfs_fileoff_t			startoff2 = req->startoff2;
	xfs_filblks_t			blockcount = req->blockcount;
	uint32_t			mod;
	int				nimaps;
	int				error;

	/* xattrs don't live on the rt device */
	if (req->whichfork == XFS_ATTR_FORK)
		return 0;

	/*
	 * Caller got permission to use SXI log items, so log recovery will
	 * finish the swap and not leave us with partially swapped rt extents
	 * exposed to userspace.
	 */
	if (req->req_flags & XFS_SWAP_REQ_LOGGED)
		return 0;

	/*
	 * Allocation units must be fully mapped to a file range.  For files
	 * with a single-fsblock allocation unit, this is trivial.
	 */
	if (!xfs_inode_has_bigallocunit(req->ip2))
		return 0;

	/*
	 * For multi-fsblock allocation units, we must check the alignment of
	 * every single mapping.
	 */
	while (blockcount > 0) {
		/* Read extent from the first file */
		nimaps = 1;
		error = xfs_bmapi_read(req->ip1, startoff1, blockcount,
				&irec1, &nimaps, 0);
		if (error)
			return error;
		ASSERT(nimaps == 1);

		/* Read extent from the second file */
		nimaps = 1;
		error = xfs_bmapi_read(req->ip2, startoff2,
				irec1.br_blockcount, &irec2, &nimaps,
				0);
		if (error)
			return error;
		ASSERT(nimaps == 1);

		/*
		 * We can only swap as many blocks as the smaller of the two
		 * extent maps.
		 */
		irec1.br_blockcount = min(irec1.br_blockcount,
					  irec2.br_blockcount);

		/* Both mappings must be aligned to the realtime extent size. */
		mod = xfs_rtb_to_rtxoff(mp, irec1.br_startoff);
		if (mod) {
			ASSERT(mod == 0);
			return -EINVAL;
		}

		mod = xfs_rtb_to_rtxoff(mp, irec1.br_startoff);
		if (mod) {
			ASSERT(mod == 0);
			return -EINVAL;
		}

		mod = xfs_rtb_to_rtxoff(mp, irec1.br_blockcount);
		if (mod) {
			ASSERT(mod == 0);
			return -EINVAL;
		}

		startoff1 += irec1.br_blockcount;
		startoff2 += irec1.br_blockcount;
		blockcount -= irec1.br_blockcount;
	}

	return 0;
}
#else
# define xfs_swapext_check_rt_extents(mp, req)		(0)
#endif

/* Check all extents to make sure we can actually swap them. */
int
xfs_swapext_check_extents(
	struct xfs_mount		*mp,
	const struct xfs_swapext_req	*req)
{
	struct xfs_ifork		*ifp1, *ifp2;

	/* No fork? */
	ifp1 = xfs_ifork_ptr(req->ip1, req->whichfork);
	ifp2 = xfs_ifork_ptr(req->ip2, req->whichfork);
	if (!ifp1 || !ifp2)
		return -EINVAL;

	/* We don't know how to swap local format forks. */
	if (ifp1->if_format == XFS_DINODE_FMT_LOCAL ||
	    ifp2->if_format == XFS_DINODE_FMT_LOCAL)
		return -EINVAL;

	return xfs_swapext_check_rt_extents(mp, req);
}

#ifdef CONFIG_XFS_QUOTA
/* Log the actual updates to the quota accounting. */
static inline void
xfs_swapext_update_quota(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi,
	struct xfs_bmbt_irec		*irec1,
	struct xfs_bmbt_irec		*irec2)
{
	int64_t				ip1_delta = 0, ip2_delta = 0;
	unsigned int			qflag;

	qflag = XFS_IS_REALTIME_INODE(sxi->sxi_ip1) ? XFS_TRANS_DQ_RTBCOUNT :
						      XFS_TRANS_DQ_BCOUNT;

	if (xfs_bmap_is_real_extent(irec1)) {
		ip1_delta -= irec1->br_blockcount;
		ip2_delta += irec1->br_blockcount;
	}

	if (xfs_bmap_is_real_extent(irec2)) {
		ip1_delta += irec2->br_blockcount;
		ip2_delta -= irec2->br_blockcount;
	}

	xfs_trans_mod_dquot_byino(tp, sxi->sxi_ip1, qflag, ip1_delta);
	xfs_trans_mod_dquot_byino(tp, sxi->sxi_ip2, qflag, ip2_delta);
}
#else
# define xfs_swapext_update_quota(tp, sxi, irec1, irec2)	((void)0)
#endif

/* Decide if we want to skip this mapping from file1. */
static inline bool
xfs_swapext_can_skip_mapping(
	struct xfs_swapext_intent	*sxi,
	struct xfs_bmbt_irec		*irec)
{
	struct xfs_mount		*mp = sxi->sxi_ip1->i_mount;

	/* Do not skip this mapping if the caller did not tell us to. */
	if (!(sxi->sxi_flags & XFS_SWAP_EXT_INO1_WRITTEN))
		return false;

	/* Do not skip mapped, written extents. */
	if (xfs_bmap_is_written_extent(irec))
		return false;

	/*
	 * The mapping is unwritten or a hole.  It cannot be a delalloc
	 * reservation because we already excluded those.  It cannot be an
	 * unwritten extent with dirty page cache because we flushed the page
	 * cache.  For files where the allocation unit is 1FSB (files on the
	 * data dev, rt files if the extent size is 1FSB), we can safely
	 * skip this mapping.
	 */
	if (!xfs_inode_has_bigallocunit(sxi->sxi_ip1))
		return true;

	/*
	 * For a realtime file with a multi-fsb allocation unit, the decision
	 * is trickier because we can only swap full allocation units.
	 * Unwritten mappings can appear in the middle of an rtx if the rtx is
	 * partially written, but they can also appear for preallocations.
	 *
	 * If the mapping is a hole, skip it entirely.  Holes should align with
	 * rtx boundaries.
	 */
	if (!xfs_bmap_is_real_extent(irec))
		return true;

	/*
	 * All mappings below this point are unwritten.
	 *
	 * - If the beginning is not aligned to an rtx, trim the end of the
	 *   mapping so that it does not cross an rtx boundary, and swap it.
	 *
	 * - If both ends are aligned to an rtx, skip the entire mapping.
	 */
	if (!isaligned_64(irec->br_startoff, mp->m_sb.sb_rextsize)) {
		xfs_fileoff_t	new_end;

		new_end = roundup_64(irec->br_startoff, mp->m_sb.sb_rextsize);
		irec->br_blockcount = min(irec->br_blockcount,
					  new_end - irec->br_startoff);
		return false;
	}
	if (isaligned_64(irec->br_blockcount, mp->m_sb.sb_rextsize))
		return true;

	/*
	 * All mappings below this point are unwritten, start on an rtx
	 * boundary, and do not end on an rtx boundary.
	 *
	 * - If the mapping is longer than one rtx, trim the end of the mapping
	 *   down to an rtx boundary and skip it.
	 *
	 * - The mapping is shorter than one rtx.  Swap it.
	 */
	if (irec->br_blockcount > mp->m_sb.sb_rextsize) {
		xfs_fileoff_t	new_end;

		new_end = rounddown_64(irec->br_startoff + irec->br_blockcount,
				mp->m_sb.sb_rextsize);
		irec->br_blockcount = new_end - irec->br_startoff;
		return true;
	}

	return false;
}

/*
 * Walk forward through the file ranges in @sxi until we find two different
 * mappings to exchange.  If there is work to do, return the mappings;
 * otherwise we've reached the end of the range and sxi_blockcount will be
 * zero.
 *
 * If the walk skips over a pair of mappings to the same storage, save them as
 * the left records in @adj (if provided) so that the simulation phase can
 * avoid an extra lookup.
  */
static int
xfs_swapext_find_mappings(
	struct xfs_swapext_intent	*sxi,
	struct xfs_bmbt_irec		*irec1,
	struct xfs_bmbt_irec		*irec2,
	struct xfs_swapext_adjacent	*adj)
{
	int				nimaps;
	int				bmap_flags;
	int				error;

	bmap_flags = xfs_bmapi_aflag(xfs_swapext_whichfork(sxi));

	for (; sxi_has_more_swap_work(sxi); sxi_advance(sxi, irec1)) {
		/* Read extent from the first file */
		nimaps = 1;
		error = xfs_bmapi_read(sxi->sxi_ip1, sxi->sxi_startoff1,
				sxi->sxi_blockcount, irec1, &nimaps,
				bmap_flags);
		if (error)
			return error;
		if (nimaps != 1 ||
		    irec1->br_startblock == DELAYSTARTBLOCK ||
		    irec1->br_startoff != sxi->sxi_startoff1) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		if (xfs_swapext_can_skip_mapping(sxi, irec1)) {
			trace_xfs_swapext_extent1_skip(sxi->sxi_ip1, irec1);
			continue;
		}

		/* Read extent from the second file */
		nimaps = 1;
		error = xfs_bmapi_read(sxi->sxi_ip2, sxi->sxi_startoff2,
				irec1->br_blockcount, irec2, &nimaps,
				bmap_flags);
		if (error)
			return error;
		if (nimaps != 1 ||
		    irec2->br_startblock == DELAYSTARTBLOCK ||
		    irec2->br_startoff != sxi->sxi_startoff2) {
			/*
			 * We should never get no mapping or a delalloc extent
			 * or something that doesn't match what we asked for,
			 * since the caller flushed both inodes and we hold the
			 * ILOCKs for both inodes.
			 */
			ASSERT(0);
			return -EINVAL;
		}

		/*
		 * We can only swap as many blocks as the smaller of the two
		 * extent maps.
		 */
		irec1->br_blockcount = min(irec1->br_blockcount,
					   irec2->br_blockcount);

		trace_xfs_swapext_extent1(sxi->sxi_ip1, irec1);
		trace_xfs_swapext_extent2(sxi->sxi_ip2, irec2);

		/* We found something to swap, so return it. */
		if (irec1->br_startblock != irec2->br_startblock)
			return 0;

		/*
		 * Two extents mapped to the same physical block must not have
		 * different states; that's filesystem corruption.  Move on to
		 * the next extent if they're both holes or both the same
		 * physical extent.
		 */
		if (irec1->br_state != irec2->br_state) {
			xfs_bmap_mark_sick(sxi->sxi_ip1,
					xfs_swapext_whichfork(sxi));
			xfs_bmap_mark_sick(sxi->sxi_ip2,
					xfs_swapext_whichfork(sxi));
			return -EFSCORRUPTED;
		}

		/*
		 * Save the mappings if we're estimating work and skipping
		 * these identical mappings.
		 */
		if (adj) {
			memcpy(&adj->left1, irec1, sizeof(*irec1));
			memcpy(&adj->left2, irec2, sizeof(*irec2));
		}
	}

	return 0;
}

/* Exchange these two mappings. */
static void
xfs_swapext_exchange_mappings(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi,
	struct xfs_bmbt_irec		*irec1,
	struct xfs_bmbt_irec		*irec2)
{
	int				whichfork = xfs_swapext_whichfork(sxi);

	xfs_swapext_update_quota(tp, sxi, irec1, irec2);

	/* Remove both mappings. */
	xfs_bmap_unmap_extent(tp, sxi->sxi_ip1, whichfork, irec1);
	xfs_bmap_unmap_extent(tp, sxi->sxi_ip2, whichfork, irec2);

	/*
	 * Re-add both mappings.  We swap the file offsets between the two maps
	 * and add the opposite map, which has the effect of filling the
	 * logical offsets we just unmapped, but with with the physical mapping
	 * information swapped.
	 */
	swap(irec1->br_startoff, irec2->br_startoff);
	xfs_bmap_map_extent(tp, sxi->sxi_ip1, whichfork, irec2);
	xfs_bmap_map_extent(tp, sxi->sxi_ip2, whichfork, irec1);

	/* Make sure we're not mapping extents past EOF. */
	if (whichfork == XFS_DATA_FORK) {
		xfs_swapext_update_size(tp, sxi->sxi_ip1, irec2,
				sxi->sxi_isize1);
		xfs_swapext_update_size(tp, sxi->sxi_ip2, irec1,
				sxi->sxi_isize2);
	}

	/*
	 * Advance our cursor and exit.   The caller (either defer ops or log
	 * recovery) will log the SXD item, and if *blockcount is nonzero, it
	 * will log a new SXI item for the remainder and call us back.
	 */
	sxi_advance(sxi, irec1);
}

/* Convert inode2's leaf attr fork back to shortform, if possible.. */
STATIC int
xfs_swapext_attr_to_sf(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	struct xfs_da_args	args = {
		.dp		= sxi->sxi_ip2,
		.geo		= tp->t_mountp->m_attr_geo,
		.whichfork	= XFS_ATTR_FORK,
		.trans		= tp,
		.owner		= sxi->sxi_ip2->i_ino,
	};
	struct xfs_buf		*bp;
	int			forkoff;
	int			error;

	if (!xfs_attr_is_leaf(sxi->sxi_ip2))
		return 0;

	error = xfs_attr3_leaf_read(tp, sxi->sxi_ip2, sxi->sxi_ip2->i_ino, 0,
			&bp);
	if (error)
		return error;

	forkoff = xfs_attr_shortform_allfit(bp, sxi->sxi_ip2);
	if (forkoff == 0)
		return 0;

	return xfs_attr3_leaf_to_shortform(bp, &args, forkoff);
}

/* Convert inode2's block dir fork back to shortform, if possible.. */
STATIC int
xfs_swapext_dir_to_sf(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	struct xfs_da_args	args = {
		.dp		= sxi->sxi_ip2,
		.geo		= tp->t_mountp->m_dir_geo,
		.whichfork	= XFS_DATA_FORK,
		.trans		= tp,
		.owner		= sxi->sxi_ip2->i_ino,
	};
	struct xfs_dir2_sf_hdr	sfh;
	struct xfs_buf		*bp;
	bool			isblock;
	int			size;
	int			error;

	error = xfs_dir2_isblock(&args, &isblock);
	if (error)
		return error;

	if (!isblock)
		return 0;

	error = xfs_dir3_block_read(tp, sxi->sxi_ip2, sxi->sxi_ip2->i_ino, &bp);
	if (error)
		return error;

	size = xfs_dir2_block_sfsize(sxi->sxi_ip2, bp->b_addr, &sfh);
	if (size > xfs_inode_data_fork_size(sxi->sxi_ip2))
		return 0;

	return xfs_dir2_block_to_sf(&args, bp, size, &sfh);
}

/* Convert inode2's remote symlink target back to shortform, if possible. */
STATIC int
xfs_swapext_link_to_sf(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	struct xfs_inode		*ip = sxi->sxi_ip2;
	struct xfs_ifork		*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	char				*buf;
	int				error;

	if (ifp->if_format == XFS_DINODE_FMT_LOCAL ||
	    ip->i_disk_size > xfs_inode_data_fork_size(ip))
		return 0;

	/* Read the current symlink target into a buffer. */
	buf = kmem_alloc(ip->i_disk_size + 1, KM_NOFS);
	if (!buf) {
		ASSERT(0);
		return -ENOMEM;
	}

	error = xfs_symlink_remote_read(ip, buf);
	if (error)
		goto free;

	/* Remove the blocks. */
	error = xfs_symlink_remote_truncate(tp, ip);
	if (error)
		goto free;

	/* Convert fork to local format and log our changes. */
	xfs_idestroy_fork(ifp);
	ifp->if_bytes = 0;
	ifp->if_format = XFS_DINODE_FMT_LOCAL;
	xfs_init_local_fork(ip, XFS_DATA_FORK, buf, ip->i_disk_size);
	xfs_trans_log_inode(tp, ip, XFS_ILOG_DDATA | XFS_ILOG_CORE);
free:
	kmem_free(buf);
	return error;
}

static inline void
xfs_swapext_clear_reflink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	trace_xfs_reflink_unset_inode_flag(ip);

	ip->i_diflags2 &= ~XFS_DIFLAG2_REFLINK;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}

/* Finish whatever work might come after a swap operation. */
static int
xfs_swapext_do_postop_work(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	if (sxi->sxi_flags & XFS_SWAP_EXT_CVT_INO2_SF) {
		int			error = 0;

		if (sxi->sxi_flags & XFS_SWAP_EXT_ATTR_FORK)
			error = xfs_swapext_attr_to_sf(tp, sxi);
		else if (S_ISDIR(VFS_I(sxi->sxi_ip2)->i_mode))
			error = xfs_swapext_dir_to_sf(tp, sxi);
		else if (S_ISLNK(VFS_I(sxi->sxi_ip2)->i_mode))
			error = xfs_swapext_link_to_sf(tp, sxi);
		sxi->sxi_flags &= ~XFS_SWAP_EXT_CVT_INO2_SF;
		if (error)
			return error;
	}

	if (sxi->sxi_flags & XFS_SWAP_EXT_CLEAR_INO1_REFLINK) {
		xfs_swapext_clear_reflink(tp, sxi->sxi_ip1);
		sxi->sxi_flags &= ~XFS_SWAP_EXT_CLEAR_INO1_REFLINK;
	}

	if (sxi->sxi_flags & XFS_SWAP_EXT_CLEAR_INO2_REFLINK) {
		xfs_swapext_clear_reflink(tp, sxi->sxi_ip2);
		sxi->sxi_flags &= ~XFS_SWAP_EXT_CLEAR_INO2_REFLINK;
	}

	return 0;
}

/* Finish one extent swap, possibly log more. */
int
xfs_swapext_finish_one(
	struct xfs_trans		*tp,
	struct xfs_swapext_intent	*sxi)
{
	struct xfs_bmbt_irec		irec1, irec2;
	int				error;

	if (sxi_has_more_swap_work(sxi)) {
		/*
		 * If the operation state says that some range of the files
		 * have not yet been swapped, look for extents in that range to
		 * swap.  If we find some extents, swap them.
		 */
		error = xfs_swapext_find_mappings(sxi, &irec1, &irec2, NULL);
		if (error)
			return error;

		if (sxi_has_more_swap_work(sxi))
			xfs_swapext_exchange_mappings(tp, sxi, &irec1, &irec2);

		/*
		 * If the caller asked us to exchange the file sizes after the
		 * swap and either we just swapped the last extents in the
		 * range or we didn't find anything to swap, update the ondisk
		 * file sizes.
		 */
		if ((sxi->sxi_flags & XFS_SWAP_EXT_SET_SIZES) &&
		    !sxi_has_more_swap_work(sxi)) {
			sxi->sxi_ip1->i_disk_size = sxi->sxi_isize1;
			sxi->sxi_ip2->i_disk_size = sxi->sxi_isize2;

			xfs_trans_log_inode(tp, sxi->sxi_ip1, XFS_ILOG_CORE);
			xfs_trans_log_inode(tp, sxi->sxi_ip2, XFS_ILOG_CORE);
		}
	} else if (sxi_has_postop_work(sxi)) {
		/*
		 * Now that we're finished with the swap operation, complete
		 * the post-op cleanup work.
		 */
		error = xfs_swapext_do_postop_work(tp, sxi);
		if (error)
			return error;
	}

	if (XFS_TEST_ERROR(false, tp->t_mountp, XFS_ERRTAG_SWAPEXT_FINISH_ONE))
		return -EIO;

	/* If we still have work to do, ask for a new transaction. */
	if (sxi_has_more_swap_work(sxi) || sxi_has_postop_work(sxi)) {
		trace_xfs_swapext_defer(tp->t_mountp, sxi);
		return -EAGAIN;
	}

	/*
	 * If we reach here, we've finished all the swapping work and the post
	 * operation work.  The last thing we need to do before returning to
	 * the caller is to make sure that COW forks are set up correctly.
	 */
	if (!(sxi->sxi_flags & XFS_SWAP_EXT_ATTR_FORK)) {
		xfs_swapext_ensure_cowfork(sxi->sxi_ip1);
		xfs_swapext_ensure_cowfork(sxi->sxi_ip2);
	}

	return 0;
}

/*
 * Compute the amount of bmbt blocks we should reserve for each file.  In the
 * worst case, each exchange will fill a hole with a new mapping, which could
 * result in a btree split every time we add a new leaf block.
 */
static inline uint64_t
xfs_swapext_bmbt_blocks(
	struct xfs_mount		*mp,
	const struct xfs_swapext_req	*req)
{
	return howmany_64(req->nr_exchanges,
					XFS_MAX_CONTIG_BMAPS_PER_BLOCK(mp)) *
			XFS_EXTENTADD_SPACE_RES(mp, req->whichfork);
}

static inline uint64_t
xfs_swapext_rmapbt_blocks(
	struct xfs_mount		*mp,
	const struct xfs_swapext_req	*req)
{
	if (!xfs_has_rmapbt(mp))
		return 0;
	if (XFS_IS_REALTIME_INODE(req->ip1))
		return 0;

	return howmany_64(req->nr_exchanges,
					XFS_MAX_CONTIG_RMAPS_PER_BLOCK(mp)) *
			XFS_RMAPADD_SPACE_RES(mp);
}

/* Estimate the bmbt and rmapbt overhead required to exchange extents. */
int
xfs_swapext_estimate_overhead(
	struct xfs_swapext_req	*req)
{
	struct xfs_mount	*mp = req->ip1->i_mount;
	xfs_filblks_t		bmbt_blocks;
	xfs_filblks_t		rmapbt_blocks;
	xfs_filblks_t		resblks = req->resblks;

	/*
	 * Compute the number of bmbt and rmapbt blocks we might need to handle
	 * the estimated number of exchanges.
	 */
	bmbt_blocks = xfs_swapext_bmbt_blocks(mp, req);
	rmapbt_blocks = xfs_swapext_rmapbt_blocks(mp, req);

	trace_xfs_swapext_overhead(mp, bmbt_blocks, rmapbt_blocks);

	/* Make sure the change in file block count doesn't overflow. */
	if (check_add_overflow(req->ip1_bcount, bmbt_blocks, &req->ip1_bcount))
		return -EFBIG;
	if (check_add_overflow(req->ip2_bcount, bmbt_blocks, &req->ip2_bcount))
		return -EFBIG;

	/*
	 * Add together the number of blocks we need to handle btree growth,
	 * then add it to the number of blocks we need to reserve to this
	 * transaction.
	 */
	if (check_add_overflow(resblks, bmbt_blocks, &resblks))
		return -ENOSPC;
	if (check_add_overflow(resblks, bmbt_blocks, &resblks))
		return -ENOSPC;
	if (check_add_overflow(resblks, rmapbt_blocks, &resblks))
		return -ENOSPC;
	if (check_add_overflow(resblks, rmapbt_blocks, &resblks))
		return -ENOSPC;

	/* Can't actually reserve more than UINT_MAX blocks. */
	if (req->resblks > UINT_MAX)
		return -ENOSPC;

	req->resblks = resblks;
	trace_xfs_swapext_final_estimate(req);
	return 0;
}

/* Decide if we can merge two real extents. */
static inline bool
can_merge(
	const struct xfs_bmbt_irec	*b1,
	const struct xfs_bmbt_irec	*b2)
{
	/* Don't merge holes. */
	if (b1->br_startblock == HOLESTARTBLOCK ||
	    b2->br_startblock == HOLESTARTBLOCK)
		return false;

	/* We don't merge holes. */
	if (!xfs_bmap_is_real_extent(b1) || !xfs_bmap_is_real_extent(b2))
		return false;

	if (b1->br_startoff   + b1->br_blockcount == b2->br_startoff &&
	    b1->br_startblock + b1->br_blockcount == b2->br_startblock &&
	    b1->br_state			  == b2->br_state &&
	    b1->br_blockcount + b2->br_blockcount <= XFS_MAX_BMBT_EXTLEN)
		return true;

	return false;
}

#define CLEFT_CONTIG	0x01
#define CRIGHT_CONTIG	0x02
#define CHOLE		0x04
#define CBOTH_CONTIG	(CLEFT_CONTIG | CRIGHT_CONTIG)

#define NLEFT_CONTIG	0x10
#define NRIGHT_CONTIG	0x20
#define NHOLE		0x40
#define NBOTH_CONTIG	(NLEFT_CONTIG | NRIGHT_CONTIG)

/* Estimate the effect of a single swap on extent count. */
static inline int
delta_nextents_step(
	struct xfs_mount		*mp,
	const struct xfs_bmbt_irec	*left,
	const struct xfs_bmbt_irec	*curr,
	const struct xfs_bmbt_irec	*new,
	const struct xfs_bmbt_irec	*right)
{
	bool				lhole, rhole, chole, nhole;
	unsigned int			state = 0;
	int				ret = 0;

	lhole = left->br_startblock == HOLESTARTBLOCK;
	rhole = right->br_startblock == HOLESTARTBLOCK;
	chole = curr->br_startblock == HOLESTARTBLOCK;
	nhole = new->br_startblock == HOLESTARTBLOCK;

	if (chole)
		state |= CHOLE;
	if (!lhole && !chole && can_merge(left, curr))
		state |= CLEFT_CONTIG;
	if (!rhole && !chole && can_merge(curr, right))
		state |= CRIGHT_CONTIG;
	if ((state & CBOTH_CONTIG) == CBOTH_CONTIG &&
	    left->br_startblock + curr->br_startblock +
					right->br_startblock > XFS_MAX_BMBT_EXTLEN)
		state &= ~CRIGHT_CONTIG;

	if (nhole)
		state |= NHOLE;
	if (!lhole && !nhole && can_merge(left, new))
		state |= NLEFT_CONTIG;
	if (!rhole && !nhole && can_merge(new, right))
		state |= NRIGHT_CONTIG;
	if ((state & NBOTH_CONTIG) == NBOTH_CONTIG &&
	    left->br_startblock + new->br_startblock +
					right->br_startblock > XFS_MAX_BMBT_EXTLEN)
		state &= ~NRIGHT_CONTIG;

	switch (state & (CLEFT_CONTIG | CRIGHT_CONTIG | CHOLE)) {
	case CLEFT_CONTIG | CRIGHT_CONTIG:
		/*
		 * left/curr/right are the same extent, so deleting curr causes
		 * 2 new extents to be created.
		 */
		ret += 2;
		break;
	case 0:
		/*
		 * curr is not contiguous with any extent, so we remove curr
		 * completely
		 */
		ret--;
		break;
	case CHOLE:
		/* hole, do nothing */
		break;
	case CLEFT_CONTIG:
	case CRIGHT_CONTIG:
		/* trim either left or right, no change */
		break;
	}

	switch (state & (NLEFT_CONTIG | NRIGHT_CONTIG | NHOLE)) {
	case NLEFT_CONTIG | NRIGHT_CONTIG:
		/*
		 * left/curr/right will become the same extent, so adding
		 * curr causes the deletion of right.
		 */
		ret--;
		break;
	case 0:
		/* new is not contiguous with any extent */
		ret++;
		break;
	case NHOLE:
		/* hole, do nothing. */
		break;
	case NLEFT_CONTIG:
	case NRIGHT_CONTIG:
		/* new is absorbed into left or right, no change */
		break;
	}

	trace_xfs_swapext_delta_nextents_step(mp, left, curr, new, right, ret,
			state);
	return ret;
}

/* Make sure we don't overflow the extent counters. */
static inline int
ensure_delta_nextents(
	struct xfs_swapext_req	*req,
	struct xfs_inode	*ip,
	int64_t			delta)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, req->whichfork);
	xfs_extnum_t		max_extents;
	bool			large_extcount;

	if (delta < 0)
		return 0;

	if (XFS_TEST_ERROR(false, mp, XFS_ERRTAG_REDUCE_MAX_IEXTENTS)) {
		if (ifp->if_nextents + delta > 10)
			return -EFBIG;
	}

	if (req->req_flags & XFS_SWAP_REQ_NREXT64)
		large_extcount = true;
	else
		large_extcount = xfs_inode_has_large_extent_counts(ip);

	max_extents = xfs_iext_max_nextents(large_extcount, req->whichfork);
	if (ifp->if_nextents + delta <= max_extents)
		return 0;
	if (large_extcount)
		return -EFBIG;
	if (!xfs_has_large_extent_counts(mp))
		return -EFBIG;

	max_extents = xfs_iext_max_nextents(true, req->whichfork);
	if (ifp->if_nextents + delta > max_extents)
		return -EFBIG;

	req->req_flags |= XFS_SWAP_REQ_NREXT64;
	return 0;
}

/* Find the next extent after irec. */
static inline int
get_next_ext(
	struct xfs_inode		*ip,
	int				bmap_flags,
	const struct xfs_bmbt_irec	*irec,
	struct xfs_bmbt_irec		*nrec)
{
	xfs_fileoff_t			off;
	xfs_filblks_t			blockcount;
	int				nimaps = 1;
	int				error;

	off = irec->br_startoff + irec->br_blockcount;
	blockcount = XFS_MAX_FILEOFF - off;
	error = xfs_bmapi_read(ip, off, blockcount, nrec, &nimaps, bmap_flags);
	if (error)
		return error;
	if (nrec->br_startblock == DELAYSTARTBLOCK ||
	    nrec->br_startoff != off) {
		/*
		 * If we don't get the extent we want, return a zero-length
		 * mapping, which our estimator function will pretend is a hole.
		 * We shouldn't get delalloc reservations.
		 */
		nrec->br_startblock = HOLESTARTBLOCK;
	}

	return 0;
}

int __init
xfs_swapext_intent_init_cache(void)
{
	xfs_swapext_intent_cache = kmem_cache_create("xfs_swapext_intent",
			sizeof(struct xfs_swapext_intent),
			0, 0, NULL);

	return xfs_swapext_intent_cache != NULL ? 0 : -ENOMEM;
}

void
xfs_swapext_intent_destroy_cache(void)
{
	kmem_cache_destroy(xfs_swapext_intent_cache);
	xfs_swapext_intent_cache = NULL;
}

/*
 * Decide if we will swap the reflink flags between the two files after the
 * swap.  The only time we want to do this is if we're exchanging all extents
 * under EOF and the inode reflink flags have different states.
 */
static inline bool
sxi_can_exchange_reflink_flags(
	const struct xfs_swapext_req	*req,
	unsigned int			reflink_state)
{
	struct xfs_mount		*mp = req->ip1->i_mount;

	if (hweight32(reflink_state) != 1)
		return false;
	if (req->startoff1 != 0 || req->startoff2 != 0)
		return false;
	if (req->blockcount != XFS_B_TO_FSB(mp, req->ip1->i_disk_size))
		return false;
	if (req->blockcount != XFS_B_TO_FSB(mp, req->ip2->i_disk_size))
		return false;
	return true;
}


/* Allocate and initialize a new incore intent item from a request. */
struct xfs_swapext_intent *
xfs_swapext_init_intent(
	const struct xfs_swapext_req	*req,
	unsigned int			*reflink_state)
{
	struct xfs_swapext_intent	*sxi;
	unsigned int			rs = 0;

	sxi = kmem_cache_zalloc(xfs_swapext_intent_cache,
			GFP_NOFS | __GFP_NOFAIL);
	INIT_LIST_HEAD(&sxi->sxi_list);
	sxi->sxi_ip1 = req->ip1;
	sxi->sxi_ip2 = req->ip2;
	sxi->sxi_startoff1 = req->startoff1;
	sxi->sxi_startoff2 = req->startoff2;
	sxi->sxi_blockcount = req->blockcount;
	sxi->sxi_isize1 = sxi->sxi_isize2 = -1;

	if (req->whichfork == XFS_ATTR_FORK)
		sxi->sxi_flags |= XFS_SWAP_EXT_ATTR_FORK;

	if (req->whichfork == XFS_DATA_FORK &&
	    (req->req_flags & XFS_SWAP_REQ_SET_SIZES)) {
		sxi->sxi_flags |= XFS_SWAP_EXT_SET_SIZES;
		sxi->sxi_isize1 = req->ip2->i_disk_size;
		sxi->sxi_isize2 = req->ip1->i_disk_size;
	}

	if (req->req_flags & XFS_SWAP_REQ_INO1_WRITTEN)
		sxi->sxi_flags |= XFS_SWAP_EXT_INO1_WRITTEN;
	if (req->req_flags & XFS_SWAP_REQ_CVT_INO2_SF)
		sxi->sxi_flags |= XFS_SWAP_EXT_CVT_INO2_SF;

	if (req->req_flags & XFS_SWAP_REQ_LOGGED)
		sxi->sxi_op_flags |= XFS_SWAP_EXT_OP_LOGGED;
	if (req->req_flags & XFS_SWAP_REQ_NREXT64)
		sxi->sxi_op_flags |= XFS_SWAP_EXT_OP_NREXT64;

	if (req->whichfork == XFS_DATA_FORK) {
		/*
		 * Record the state of each inode's reflink flag before the
		 * operation.
		 */
		if (xfs_is_reflink_inode(req->ip1))
			rs |= XFS_REFLINK_STATE_IP1;
		if (xfs_is_reflink_inode(req->ip2))
			rs |= XFS_REFLINK_STATE_IP2;

		/*
		 * Figure out if we're clearing the reflink flags (which
		 * effectively swaps them) after the operation.
		 */
		if (sxi_can_exchange_reflink_flags(req, rs)) {
			if (rs & XFS_REFLINK_STATE_IP1)
				sxi->sxi_flags |=
						XFS_SWAP_EXT_CLEAR_INO1_REFLINK;
			if (rs & XFS_REFLINK_STATE_IP2)
				sxi->sxi_flags |=
						XFS_SWAP_EXT_CLEAR_INO2_REFLINK;
		}
	}

	if (reflink_state)
		*reflink_state = rs;
	return sxi;
}

/*
 * Estimate the number of exchange operations and the number of file blocks
 * in each file that will be affected by the exchange operation.
 */
int
xfs_swapext_estimate(
	struct xfs_swapext_req		*req)
{
	struct xfs_swapext_intent	*sxi;
	struct xfs_bmbt_irec		irec1, irec2;
	struct xfs_swapext_adjacent	adj = ADJACENT_INIT;
	xfs_filblks_t			ip1_blocks = 0, ip2_blocks = 0;
	int64_t				d_nexts1, d_nexts2;
	int				bmap_flags;
	int				error;

	ASSERT(!(req->req_flags & ~XFS_SWAP_REQ_FLAGS));

	bmap_flags = xfs_bmapi_aflag(req->whichfork);
	sxi = xfs_swapext_init_intent(req, NULL);

	/*
	 * To guard against the possibility of overflowing the extent counters,
	 * we have to estimate an upper bound on the potential increase in that
	 * counter.  We can split the extent at each end of the range, and for
	 * each step of the swap we can split the extent that we're working on
	 * if the extents do not align.
	 */
	d_nexts1 = d_nexts2 = 3;

	while (sxi_has_more_swap_work(sxi)) {
		/*
		 * Walk through the file ranges until we find something to
		 * swap.  Because we're simulating the swap, pass in adj to
		 * capture skipped mappings for correct estimation of bmbt
		 * record merges.
		 */
		error = xfs_swapext_find_mappings(sxi, &irec1, &irec2, &adj);
		if (error)
			goto out_free;
		if (!sxi_has_more_swap_work(sxi))
			break;

		/* Update accounting. */
		if (xfs_bmap_is_real_extent(&irec1))
			ip1_blocks += irec1.br_blockcount;
		if (xfs_bmap_is_real_extent(&irec2))
			ip2_blocks += irec2.br_blockcount;
		req->nr_exchanges++;

		/* Read the next extents from both files. */
		error = get_next_ext(req->ip1, bmap_flags, &irec1, &adj.right1);
		if (error)
			goto out_free;

		error = get_next_ext(req->ip2, bmap_flags, &irec2, &adj.right2);
		if (error)
			goto out_free;

		/* Update extent count deltas. */
		d_nexts1 += delta_nextents_step(req->ip1->i_mount,
				&adj.left1, &irec1, &irec2, &adj.right1);

		d_nexts2 += delta_nextents_step(req->ip1->i_mount,
				&adj.left2, &irec2, &irec1, &adj.right2);

		/* Now pretend we swapped the extents. */
		if (can_merge(&adj.left2, &irec1))
			adj.left2.br_blockcount += irec1.br_blockcount;
		else
			memcpy(&adj.left2, &irec1, sizeof(irec1));

		if (can_merge(&adj.left1, &irec2))
			adj.left1.br_blockcount += irec2.br_blockcount;
		else
			memcpy(&adj.left1, &irec2, sizeof(irec2));

		sxi_advance(sxi, &irec1);
	}

	/* Account for the blocks that are being exchanged. */
	if (XFS_IS_REALTIME_INODE(req->ip1) &&
	    req->whichfork == XFS_DATA_FORK) {
		req->ip1_rtbcount = ip1_blocks;
		req->ip2_rtbcount = ip2_blocks;
	} else {
		req->ip1_bcount = ip1_blocks;
		req->ip2_bcount = ip2_blocks;
	}

	/*
	 * Make sure that both forks have enough slack left in their extent
	 * counters that the swap operation will not overflow.
	 */
	trace_xfs_swapext_delta_nextents(req, d_nexts1, d_nexts2);
	if (req->ip1 == req->ip2) {
		error = ensure_delta_nextents(req, req->ip1,
				d_nexts1 + d_nexts2);
	} else {
		error = ensure_delta_nextents(req, req->ip1, d_nexts1);
		if (error)
			goto out_free;
		error = ensure_delta_nextents(req, req->ip2, d_nexts2);
	}
	if (error)
		goto out_free;

	trace_xfs_swapext_initial_estimate(req);
	error = xfs_swapext_estimate_overhead(req);
out_free:
	kmem_cache_free(xfs_swapext_intent_cache, sxi);
	return error;
}

static inline void
xfs_swapext_set_reflink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	trace_xfs_reflink_set_inode_flag(ip);

	ip->i_diflags2 |= XFS_DIFLAG2_REFLINK;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}

/*
 * If either file has shared blocks and we're swapping data forks, we must flag
 * the other file as having shared blocks so that we get the shared-block rmap
 * functions if we need to fix up the rmaps.
 */
void
xfs_swapext_ensure_reflink(
	struct xfs_trans		*tp,
	const struct xfs_swapext_intent	*sxi,
	unsigned int			reflink_state)
{
	if ((reflink_state & XFS_REFLINK_STATE_IP1) &&
	    !xfs_is_reflink_inode(sxi->sxi_ip2))
		xfs_swapext_set_reflink(tp, sxi->sxi_ip2);

	if ((reflink_state & XFS_REFLINK_STATE_IP2) &&
	    !xfs_is_reflink_inode(sxi->sxi_ip1))
		xfs_swapext_set_reflink(tp, sxi->sxi_ip1);
}

/* Widen the extent counts of both inodes if necessary. */
static inline void
xfs_swapext_upgrade_extent_counts(
	struct xfs_trans		*tp,
	const struct xfs_swapext_intent	*sxi)
{
	if (!(sxi->sxi_op_flags & XFS_SWAP_EXT_OP_NREXT64))
		return;

	sxi->sxi_ip1->i_diflags2 |= XFS_DIFLAG2_NREXT64;
	xfs_trans_log_inode(tp, sxi->sxi_ip1, XFS_ILOG_CORE);

	sxi->sxi_ip2->i_diflags2 |= XFS_DIFLAG2_NREXT64;
	xfs_trans_log_inode(tp, sxi->sxi_ip2, XFS_ILOG_CORE);
}

/*
 * Schedule a swap a range of extents from one inode to another.  If the atomic
 * swap feature is enabled, then the operation progress can be resumed even if
 * the system goes down.  The caller must commit the transaction to start the
 * work.
 *
 * The caller must ensure the inodes must be joined to the transaction and
 * ILOCKd; they will still be joined to the transaction at exit.
 */
void
xfs_swapext(
	struct xfs_trans		*tp,
	const struct xfs_swapext_req	*req)
{
	struct xfs_swapext_intent	*sxi;
	unsigned int			reflink_state;

	ASSERT(xfs_isilocked(req->ip1, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(req->ip2, XFS_ILOCK_EXCL));
	ASSERT(req->whichfork != XFS_COW_FORK);
	ASSERT(!(req->req_flags & ~XFS_SWAP_REQ_FLAGS));
	if (req->req_flags & XFS_SWAP_REQ_SET_SIZES)
		ASSERT(req->whichfork == XFS_DATA_FORK);
	if (req->req_flags & XFS_SWAP_REQ_CVT_INO2_SF)
		ASSERT(req->whichfork == XFS_ATTR_FORK ||
		       (req->whichfork == XFS_DATA_FORK &&
			(S_ISDIR(VFS_I(req->ip2)->i_mode) ||
			 S_ISLNK(VFS_I(req->ip2)->i_mode))));

	if (req->blockcount == 0)
		return;

	sxi = xfs_swapext_init_intent(req, &reflink_state);
	xfs_swapext_defer_add(tp, sxi);
	xfs_swapext_ensure_reflink(tp, sxi, reflink_state);
	xfs_swapext_upgrade_extent_counts(tp, sxi);
}
