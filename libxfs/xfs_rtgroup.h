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

#ifdef CONFIG_XFS_RT
xfs_rgblock_t xfs_rtgroup_block_count(struct xfs_mount *mp,
		xfs_rgnumber_t rgno);
#else
# define xfs_rtgroup_block_count(mp, rgno)	(0)
#endif /* CONFIG_XFS_RT */

#endif /* __LIBXFS_RTGROUP_H */
