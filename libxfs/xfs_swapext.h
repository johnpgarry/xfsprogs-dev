/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_SWAPEXT_H_
#define __XFS_SWAPEXT_H_ 1

/*
 * Decide if this filesystem supports the minimum feature set required to use
 * the swapext iteration code in non-atomic swap mode.  This mode uses the
 * BUI log items introduced for the rmapbt and reflink features, but does not
 * use swapext log items to track progress over a file range.
 */
static inline bool
xfs_swapext_supports_nonatomic(
	struct xfs_mount	*mp)
{
	return xfs_has_reflink(mp) || xfs_has_rmapbt(mp);
}

/*
 * Decide if this filesystem has a new enough permanent feature set to protect
 * swapext log items from being replayed on a kernel that does not have
 * XFS_SB_FEAT_INCOMPAT_LOG_SWAPEXT set.
 */
static inline bool
xfs_swapext_can_use_without_log_assistance(
	struct xfs_mount	*mp)
{
	if (!xfs_sb_is_v5(&mp->m_sb))
		return false;

	if (xfs_sb_has_incompat_feature(&mp->m_sb,
				~(XFS_SB_FEAT_INCOMPAT_FTYPE |
				  XFS_SB_FEAT_INCOMPAT_SPINODES |
				  XFS_SB_FEAT_INCOMPAT_META_UUID |
				  XFS_SB_FEAT_INCOMPAT_BIGTIME |
				  XFS_SB_FEAT_INCOMPAT_NREXT64)))
		return true;

	return false;
}

/*
 * Decide if atomic extent swapping could be used on this filesystem.  This
 * does not say anything about the filesystem's readiness to do that.
 */
static inline bool
xfs_atomic_swap_supported(
	struct xfs_mount	*mp)
{
	/*
	 * In theory, we could support atomic extent swapping by setting
	 * XFS_SB_FEAT_INCOMPAT_LOG_SWAPEXT on any filesystem and that would be
	 * sufficient to protect the swapext log items that would be created.
	 * However, we don't want to enable new features on a really old
	 * filesystem, so we'll only advertise atomic swap support on the ones
	 * that support BUI log items.
	 */
	if (xfs_swapext_supports_nonatomic(mp))
		return true;

	/*
	 * If the filesystem has an RO_COMPAT or INCOMPAT bit that we don't
	 * recognize, then it's new enough not to need INCOMPAT_LOG_SWAPEXT
	 * to protect swapext log items.
	 */
	if (xfs_swapext_can_use_without_log_assistance(mp))
		return true;

	return false;
}

#endif /* __XFS_SWAPEXT_H_ */
