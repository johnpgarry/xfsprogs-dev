// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs_priv.h"
#include "libxfs.h"
#include "libxfs_io.h"
#include "init.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_ialloc.h"
#include "xfs_alloc.h"
#include "xfs_bit.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2_priv.h"
#include "xfs_health.h"

/*
 * Calculate the worst case log unit reservation for a given superblock
 * configuration. Copied and munged from the kernel code, and assumes a
 * worse case header usage (maximum log buffer sizes)
 */
int
xfs_log_calc_unit_res(
	struct xfs_mount	*mp,
	int			unit_bytes)
{
	int			iclog_space;
	int			iclog_header_size;
	int			iclog_size;
	uint			num_headers;

	if (xfs_has_logv2(mp)) {
		iclog_size = XLOG_MAX_RECORD_BSIZE;
		iclog_header_size = BBTOB(iclog_size / XLOG_HEADER_CYCLE_SIZE);
	} else {
		iclog_size = XLOG_BIG_RECORD_BSIZE;
		iclog_header_size = BBSIZE;
	}

	/*
	 * Permanent reservations have up to 'cnt'-1 active log operations
	 * in the log.  A unit in this case is the amount of space for one
	 * of these log operations.  Normal reservations have a cnt of 1
	 * and their unit amount is the total amount of space required.
	 *
	 * The following lines of code account for non-transaction data
	 * which occupy space in the on-disk log.
	 *
	 * Normal form of a transaction is:
	 * <oph><trans-hdr><start-oph><reg1-oph><reg1><reg2-oph>...<commit-oph>
	 * and then there are LR hdrs, split-recs and roundoff at end of syncs.
	 *
	 * We need to account for all the leadup data and trailer data
	 * around the transaction data.
	 * And then we need to account for the worst case in terms of using
	 * more space.
	 * The worst case will happen if:
	 * - the placement of the transaction happens to be such that the
	 *   roundoff is at its maximum
	 * - the transaction data is synced before the commit record is synced
	 *   i.e. <transaction-data><roundoff> | <commit-rec><roundoff>
	 *   Therefore the commit record is in its own Log Record.
	 *   This can happen as the commit record is called with its
	 *   own region to xlog_write().
	 *   This then means that in the worst case, roundoff can happen for
	 *   the commit-rec as well.
	 *   The commit-rec is smaller than padding in this scenario and so it is
	 *   not added separately.
	 */

	/* for trans header */
	unit_bytes += sizeof(xlog_op_header_t);
	unit_bytes += sizeof(xfs_trans_header_t);

	/* for start-rec */
	unit_bytes += sizeof(xlog_op_header_t);

	/*
	 * for LR headers - the space for data in an iclog is the size minus
	 * the space used for the headers. If we use the iclog size, then we
	 * undercalculate the number of headers required.
	 *
	 * Furthermore - the addition of op headers for split-recs might
	 * increase the space required enough to require more log and op
	 * headers, so take that into account too.
	 *
	 * IMPORTANT: This reservation makes the assumption that if this
	 * transaction is the first in an iclog and hence has the LR headers
	 * accounted to it, then the remaining space in the iclog is
	 * exclusively for this transaction.  i.e. if the transaction is larger
	 * than the iclog, it will be the only thing in that iclog.
	 * Fundamentally, this means we must pass the entire log vector to
	 * xlog_write to guarantee this.
	 */
	iclog_space = iclog_size - iclog_header_size;
	num_headers = howmany(unit_bytes, iclog_space);

	/* for split-recs - ophdrs added when data split over LRs */
	unit_bytes += sizeof(xlog_op_header_t) * num_headers;

	/* add extra header reservations if we overrun */
	while (!num_headers ||
	       howmany(unit_bytes, iclog_space) > num_headers) {
		unit_bytes += sizeof(xlog_op_header_t);
		num_headers++;
	}
	unit_bytes += iclog_header_size * num_headers;

	/* for commit-rec LR header - note: padding will subsume the ophdr */
	unit_bytes += iclog_header_size;

	/* for roundoff padding for transaction data and one for commit record */
	if (xfs_has_logv2(mp) && mp->m_sb.sb_logsunit > 1) {
		/* log su roundoff */
		unit_bytes += 2 * mp->m_sb.sb_logsunit;
	} else {
		/* BB roundoff */
		unit_bytes += 2 * BBSIZE;
	}

	return unit_bytes;
}

struct timespec64
current_time(struct inode *inode)
{
	struct timespec64	tv;
	struct timeval		stv;

	gettimeofday(&stv, (struct timezone *)0);
	tv.tv_sec = stv.tv_sec;
	tv.tv_nsec = stv.tv_usec * 1000;

	return tv;
}

int
libxfs_mod_incore_sb(
	struct xfs_mount *mp,
	int		field,
	int64_t		delta,
	int		rsvd)
{
	long long	lcounter;	/* long counter for 64 bit fields */

	switch (field) {
	case XFS_TRANS_SB_FDBLOCKS:
		lcounter = (long long)mp->m_sb.sb_fdblocks;
		lcounter += delta;
		if (lcounter < 0)
			return -ENOSPC;
		mp->m_sb.sb_fdblocks = lcounter;
		return 0;
	default:
		ASSERT(0);
		return -EINVAL;
	}
}

/*
 * This routine allocates disk space for the given file.
 * Originally derived from xfs_alloc_file_space().
 */
int
libxfs_alloc_file_space(
	xfs_inode_t	*ip,
	xfs_off_t	offset,
	xfs_off_t	len,
	int		alloc_type,
	int		attr_flags)
{
	xfs_mount_t	*mp;
	xfs_off_t	count;
	xfs_filblks_t	datablocks;
	xfs_filblks_t	allocated_fsb;
	xfs_filblks_t	allocatesize_fsb;
	xfs_bmbt_irec_t *imapp;
	xfs_bmbt_irec_t imaps[1];
	int		reccount;
	uint		resblks;
	xfs_fileoff_t	startoffset_fsb;
	xfs_trans_t	*tp;
	int		xfs_bmapi_flags;
	int		error;

	if (len <= 0)
		return -EINVAL;

	count = len;
	error = 0;
	imapp = &imaps[0];
	reccount = 1;
	xfs_bmapi_flags = alloc_type ? XFS_BMAPI_PREALLOC : 0;
	mp = ip->i_mount;
	startoffset_fsb = XFS_B_TO_FSBT(mp, offset);
	allocatesize_fsb = XFS_B_TO_FSB(mp, count);

	/* allocate file space until done or until there is an error */
	while (allocatesize_fsb && !error) {
		datablocks = allocatesize_fsb;

		resblks = (uint)XFS_DIOSTRAT_SPACE_RES(mp, datablocks);
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks,
					0, 0, &tp);
		/*
		 * Check for running out of space
		 */
		if (error) {
			ASSERT(error == -ENOSPC);
			break;
		}
		xfs_trans_ijoin(tp, ip, 0);

		error = xfs_bmapi_write(tp, ip, startoffset_fsb, allocatesize_fsb,
				xfs_bmapi_flags, 0, imapp, &reccount);

		if (error)
			goto error0;

		/*
		 * Complete the transaction
		 */
		error = xfs_trans_commit(tp);
		if (error)
			break;

		allocated_fsb = imapp->br_blockcount;
		if (reccount == 0)
			return -ENOSPC;

		startoffset_fsb += allocated_fsb;
		allocatesize_fsb -= allocated_fsb;
	}
	return error;

error0:	/* Cancel bmap, cancel trans */
	xfs_trans_cancel(tp);
	return error;
}

void
cmn_err(int level, char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputs("\n", stderr);
	va_end(ap);
}

/*
 * Warnings specifically for verifier errors.  Differentiate CRC vs. invalid
 * values, and omit the stack trace unless the error level is tuned high.
 */
void
xfs_verifier_error(
	struct xfs_buf		*bp,
	int			error,
	xfs_failaddr_t		failaddr)
{
	xfs_buf_ioerror(bp, error);

	xfs_alert(NULL, "Metadata %s detected at %p, %s block 0x%llx/0x%x",
		  bp->b_error == -EFSBADCRC ? "CRC error" : "corruption",
		  failaddr ? failaddr : __return_address,
		  bp->b_ops->name, xfs_buf_daddr(bp), BBTOB(bp->b_length));
}

/*
 * Warnings for inode corruption problems.  Don't bother with the stack
 * trace unless the error level is turned up high.
 */
void
xfs_inode_verifier_error(
	struct xfs_inode	*ip,
	int			error,
	const char		*name,
	void			*buf,
	size_t			bufsz,
	xfs_failaddr_t		failaddr)
{
	xfs_alert(NULL, "Metadata %s detected at %p, inode 0x%llx %s",
		  error == -EFSBADCRC ? "CRC error" : "corruption",
		  failaddr ? failaddr : __return_address,
		  ip->i_ino, name);
}

/*
 * Complain about the kinds of metadata corruption that we can't detect from a
 * verifier, such as incorrect inter-block relationship data.  Does not set
 * bp->b_error.
 */
void
xfs_buf_corruption_error(
	struct xfs_buf		*bp,
	xfs_failaddr_t		fa)
{
	xfs_alert(NULL, "Metadata corruption detected at %p, %s block 0x%llx",
		  fa, bp->b_ops->name, xfs_buf_daddr(bp));
}

/*
 * This is called from I/O verifiers on v5 superblock filesystems. In the
 * kernel, it validates the metadata LSN parameter against the current LSN of
 * the active log. We don't have an active log in userspace so this kind of
 * validation is not required. Therefore, this function always returns true in
 * userspace.
 *
 * xfs_repair piggybacks off this mechanism to help track the largest metadata
 * LSN in use on a filesystem. Keep a record of the largest LSN seen such that
 * repair can validate it against the state of the log.
 */
xfs_lsn_t	libxfs_max_lsn = 0;
static pthread_mutex_t	libxfs_max_lsn_lock = PTHREAD_MUTEX_INITIALIZER;

bool
xfs_log_check_lsn(
	struct xfs_mount	*mp,
	xfs_lsn_t		lsn)
{
	int			cycle = CYCLE_LSN(lsn);
	int			block = BLOCK_LSN(lsn);
	int			max_cycle;
	int			max_block;

	if (lsn == NULLCOMMITLSN)
		return true;

	pthread_mutex_lock(&libxfs_max_lsn_lock);

	max_cycle = CYCLE_LSN(libxfs_max_lsn);
	max_block = BLOCK_LSN(libxfs_max_lsn);

	if ((cycle > max_cycle) ||
	    (cycle == max_cycle && block > max_block))
		libxfs_max_lsn = lsn;

	pthread_mutex_unlock(&libxfs_max_lsn_lock);

	return true;
}

void
xfs_log_item_init(
	struct xfs_mount	*mp,
	struct xfs_log_item	*item,
	int			type,
	const struct xfs_item_ops *ops)
{
	item->li_mountp = mp; 
	item->li_type = type;
	item->li_ops = ops;

	INIT_LIST_HEAD(&item->li_trans);
	INIT_LIST_HEAD(&item->li_bio_list);
}   

static struct xfs_buftarg *
xfs_find_bdev_for_inode(
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = ip->i_mount;

	if (XFS_IS_REALTIME_INODE(ip))
		return mp->m_rtdev_targp;
	return mp->m_ddev_targp;
}

static xfs_daddr_t
xfs_fsb_to_db(struct xfs_inode *ip, xfs_fsblock_t fsb)
{
	if (XFS_IS_REALTIME_INODE(ip))
		 return XFS_FSB_TO_BB(ip->i_mount, fsb);
	return XFS_FSB_TO_DADDR(ip->i_mount, (fsb));
}

int
libxfs_zero_extent(
	struct xfs_inode *ip,
	xfs_fsblock_t	start_fsb,
	xfs_off_t	count_fsb)
{
	xfs_daddr_t	sector = xfs_fsb_to_db(ip, start_fsb);
	ssize_t		size = XFS_FSB_TO_BB(ip->i_mount, count_fsb);

	return libxfs_device_zero(xfs_find_bdev_for_inode(ip), sector, size);
}

unsigned int
hweight8(unsigned int w)
{
	unsigned int res = w - ((w >> 1) & 0x55);
	res = (res & 0x33) + ((res >> 2) & 0x33);
	return (res + (res >> 4)) & 0x0F;
}

unsigned int
hweight32(unsigned int w)
{
	unsigned int res = w - ((w >> 1) & 0x55555555);
	res = (res & 0x33333333) + ((res >> 2) & 0x33333333);
	res = (res + (res >> 4)) & 0x0F0F0F0F;
	res = res + (res >> 8);
	return (res + (res >> 16)) & 0x000000FF;
}

unsigned int
hweight64(__u64 w)
{
	return hweight32((unsigned int)w) +
	       hweight32((unsigned int)(w >> 32));
}

/* xfs_health.c */

/* Mark a per-fs metadata healed. */
void
xfs_fs_mark_healthy(
	struct xfs_mount	*mp,
	unsigned int		mask)
{
	ASSERT(!(mask & ~XFS_SICK_FS_PRIMARY));
	trace_xfs_fs_mark_healthy(mp, mask);

	spin_lock(&mp->m_sb_lock);
	mp->m_fs_sick &= ~mask;
	mp->m_fs_checked |= mask;
	spin_unlock(&mp->m_sb_lock);
}

void xfs_ag_geom_health(struct xfs_perag *pag, struct xfs_ag_geometry *ageo) { }
void xfs_fs_mark_sick(struct xfs_mount *mp, unsigned int mask) { }
void xfs_agno_mark_sick(struct xfs_mount *mp, xfs_agnumber_t agno,
		unsigned int mask) { }
void xfs_ag_mark_sick(struct xfs_perag *pag, unsigned int mask) { }
void xfs_ag_measure_sickness(struct xfs_perag *pag, unsigned int *sick,
		unsigned int *checked)
{
	*sick = 0;
	*checked = 0;
}
void xfs_bmap_mark_sick(struct xfs_inode *ip, int whichfork) { }
void xfs_btree_mark_sick(struct xfs_btree_cur *cur) { }
void xfs_dirattr_mark_sick(struct xfs_inode *ip, int whichfork) { }
void xfs_da_mark_sick(struct xfs_da_args *args) { }
void xfs_inode_mark_sick(struct xfs_inode *ip, unsigned int mask) { }
void xfs_rt_mark_sick(struct xfs_mount *mp, unsigned int mask) { }

#ifdef HAVE_GETRANDOM_NONBLOCK
uint32_t
get_random_u32(void)
{
	uint32_t	ret;
	ssize_t		sz;

	/*
	 * Try to extract a u32 of randomness from /dev/urandom.  If that
	 * fails, fall back to returning zero like we used to do.
	 */
	sz = getrandom(&ret, sizeof(ret), GRND_NONBLOCK);
	if (sz != sizeof(ret))
		return 0;

	return ret;
}
#endif
