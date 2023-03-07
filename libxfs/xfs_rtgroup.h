/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBXFS_RTGROUP_H
#define __LIBXFS_RTGROUP_H 1

struct xfs_mount;
struct xfs_trans;

/*
 * Realtime group incore structure, similar to the per-AG structure.
 */
struct xfs_rtgroup {
	struct xfs_mount	*rtg_mount;
	xfs_rgnumber_t		rtg_rgno;
	atomic_t		rtg_ref;	/* passive reference count */
	atomic_t		rtg_active_ref;	/* active reference count */
	wait_queue_head_t	rtg_active_wq;/* woken active_ref falls to zero */

	/* for rcu-safe freeing */
	struct rcu_head		rcu_head;

	/* Number of blocks in this group */
	xfs_rgblock_t		rtg_blockcount;

	/*
	 * Bitsets of per-rtgroup metadata that have been checked and/or are
	 * sick.  Callers should hold rtg_state_lock before accessing this
	 * field.
	 */
	uint16_t		rtg_checked;
	uint16_t		rtg_sick;

#ifdef __KERNEL__
	/* -- kernel only structures below this line -- */
	spinlock_t		rtg_state_lock;
#endif /* __KERNEL__ */
};

#ifdef CONFIG_XFS_RT
/* Passive rtgroup references */
struct xfs_rtgroup *xfs_rtgroup_get(struct xfs_mount *mp, xfs_rgnumber_t rgno);
struct xfs_rtgroup *xfs_rtgroup_hold(struct xfs_rtgroup *rtg);
void xfs_rtgroup_put(struct xfs_rtgroup *rtg);

/* Active rtgroup references */
struct xfs_rtgroup *xfs_rtgroup_grab(struct xfs_mount *mp, xfs_rgnumber_t rgno);
void xfs_rtgroup_rele(struct xfs_rtgroup *rtg);

int xfs_initialize_rtgroups(struct xfs_mount *mp, xfs_rgnumber_t rgcount);
void xfs_free_rtgroups(struct xfs_mount *mp);
#else
static inline struct xfs_rtgroup *
xfs_rtgroup_get(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	return NULL;
}
static inline struct xfs_rtgroup *
xfs_rtgroup_hold(struct xfs_rtgroup *rtg)
{
	ASSERT(rtg == NULL);
	return NULL;
}
# define xfs_rtgroup_grab			xfs_rtgroup_get
# define xfs_rtgroup_put(rtg)			((void)0)
# define xfs_rtgroup_rele(rtg)			((void)0)
# define xfs_initialize_rtgroups(mp, rgcount)	(0)
# define xfs_free_rtgroups(mp)			((void)0)
#endif /* CONFIG_XFS_RT */

/*
 * rt group iteration APIs
 */
static inline struct xfs_rtgroup *
xfs_rtgroup_next(
	struct xfs_rtgroup	*rtg,
	xfs_rgnumber_t		*rgno,
	xfs_rgnumber_t		end_rgno)
{
	struct xfs_mount	*mp = rtg->rtg_mount;

	*rgno = rtg->rtg_rgno + 1;
	xfs_rtgroup_rele(rtg);
	if (*rgno > end_rgno)
		return NULL;
	return xfs_rtgroup_grab(mp, *rgno);
}

#define for_each_rtgroup_range(mp, rgno, end_rgno, rtg) \
	for ((rtg) = xfs_rtgroup_grab((mp), (rgno)); \
		(rtg) != NULL; \
		(rtg) = xfs_rtgroup_next((rtg), &(rgno), (end_rgno)))

#define for_each_rtgroup_from(mp, rgno, rtg) \
	for_each_rtgroup_range((mp), (rgno), (mp)->m_sb.sb_rgcount - 1, (rtg))


#define for_each_rtgroup(mp, rgno, rtg) \
	(rgno) = 0; \
	for_each_rtgroup_from((mp), (rgno), (rtg))

static inline bool
xfs_verify_rgbno(
	struct xfs_rtgroup	*rtg,
	xfs_rgblock_t		rgbno)
{
	if (rgbno >= rtg->rtg_blockcount)
		return false;
	if (rgbno < rtg->rtg_mount->m_sb.sb_rextsize)
		return false;
	return true;
}

static inline bool
xfs_verify_rgbext(
	struct xfs_rtgroup	*rtg,
	xfs_rgblock_t		rgbno,
	xfs_rgblock_t		len)
{
	if (rgbno + len <= rgbno)
		return false;

	if (!xfs_verify_rgbno(rtg, rgbno))
		return false;

	return xfs_verify_rgbno(rtg, rgbno + len - 1);
}

static inline xfs_rtblock_t
xfs_rgbno_to_rtb(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno,
	xfs_rgblock_t		rgbno)
{
	ASSERT(xfs_has_rtgroups(mp));

	if (mp->m_rgblklog >= 0)
		return ((xfs_rtblock_t)rgno << mp->m_rgblklog) | rgbno;

	return ((xfs_rtblock_t)rgno * mp->m_sb.sb_rgblocks) + rgbno;
}

static inline xfs_rgnumber_t
xfs_rtb_to_rgno(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtbno)
{
	ASSERT(xfs_has_rtgroups(mp));

	if (mp->m_rgblklog >= 0)
		return rtbno >> mp->m_rgblklog;

	return div_u64(rtbno, mp->m_sb.sb_rgblocks);
}

static inline xfs_rgblock_t
xfs_rtb_to_rgbno(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtbno,
	xfs_rgnumber_t		*rgno)
{
	uint32_t		rem;

	ASSERT(xfs_has_rtgroups(mp));

	if (mp->m_rgblklog >= 0) {
		*rgno = rtbno >> mp->m_rgblklog;
		return rtbno & mp->m_rgblkmask;
	}

	*rgno = div_u64_rem(rtbno, mp->m_sb.sb_rgblocks, &rem);
	return rem;
}

static inline xfs_daddr_t
xfs_rtb_to_daddr(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtbno)
{
	return rtbno << mp->m_blkbb_log;
}

static inline xfs_rtblock_t
xfs_daddr_to_rtb(
	struct xfs_mount	*mp,
	xfs_daddr_t		daddr)
{
	return daddr >> mp->m_blkbb_log;
}

static inline xfs_rgnumber_t
xfs_daddr_to_rgno(
	struct xfs_mount	*mp,
	xfs_daddr_t		daddr)
{
	xfs_rtblock_t		rtb = daddr >> mp->m_blkbb_log;

	return xfs_rtb_to_rgno(mp, rtb);
}

static inline xfs_rgblock_t
xfs_daddr_to_rgbno(
	struct xfs_mount	*mp,
	xfs_daddr_t		daddr)
{
	xfs_rtblock_t		rtb = daddr >> mp->m_blkbb_log;
	xfs_rgnumber_t		rgno;

	return xfs_rtb_to_rgbno(mp, rtb, &rgno);
}

#ifdef CONFIG_XFS_RT
xfs_rgblock_t xfs_rtgroup_block_count(struct xfs_mount *mp,
		xfs_rgnumber_t rgno);

void xfs_rtgroup_update_super(struct xfs_buf *rtsb_bp,
		const struct xfs_buf *sb_bp);
void xfs_rtgroup_log_super(struct xfs_trans *tp, const struct xfs_buf *sb_bp);
int xfs_rtgroup_update_secondary_sbs(struct xfs_mount *mp);
int xfs_rtgroup_init_secondary_super(struct xfs_mount *mp, xfs_rgnumber_t rgno,
		struct xfs_buf **bpp);

/* Lock the rt bitmap inode in exclusive mode */
#define XFS_RTGLOCK_BITMAP		(1U << 0)
/* Lock the rt bitmap inode in shared mode */
#define XFS_RTGLOCK_BITMAP_SHARED	(1U << 1)

#define XFS_RTGLOCK_ALL_FLAGS	(XFS_RTGLOCK_BITMAP | \
				 XFS_RTGLOCK_BITMAP_SHARED)

void xfs_rtgroup_lock(struct xfs_trans *tp, struct xfs_rtgroup *rtg,
		unsigned int rtglock_flags);
void xfs_rtgroup_unlock(struct xfs_rtgroup *rtg, unsigned int rtglock_flags);

int xfs_rtgroup_get_geometry(struct xfs_rtgroup *rtg,
		struct xfs_rtgroup_geometry *rgeo);
#else
# define xfs_rtgroup_block_count(mp, rgno)	(0)
# define xfs_rtgroup_update_super(bp, sb_bp)	((void)0)
# define xfs_rtgroup_log_super(tp, sb_bp)	((void)0)
# define xfs_rtgroup_update_secondary_sbs(mp)	(0)
# define xfs_rtgroup_init_secondary_super(mp, rgno, bpp)	(-EOPNOTSUPP)
# define xfs_rtgroup_lock(tp, rtg, gf)		((void)0)
# define xfs_rtgroup_unlock(rtg, gf)		((void)0)
# define xfs_rtgroup_get_geometry(rtg, rgeo)	(-EOPNOTSUPP)
#endif /* CONFIG_XFS_RT */

#endif /* __LIBXFS_RTGROUP_H */
