// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef	__LIBXFS_DEFER_ITEM_H_
#define	__LIBXFS_DEFER_ITEM_H_

struct xfs_bmap_intent;

void xfs_bmap_defer_add(struct xfs_trans *tp, struct xfs_bmap_intent *bi);

struct xfs_swapext_intent;

void xfs_swapext_defer_add(struct xfs_trans *tp, struct xfs_swapext_intent *sxi);

struct xfs_extent_free_item;

void xfs_extent_free_defer_add(struct xfs_trans *tp,
		struct xfs_extent_free_item *xefi,
		struct xfs_defer_pending **dfpp);

#endif /* __LIBXFS_DEFER_ITEM_H_ */
