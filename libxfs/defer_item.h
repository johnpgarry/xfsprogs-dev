// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef	__LIBXFS_DEFER_ITEM_H_
#define	__LIBXFS_DEFER_ITEM_H_

struct xfs_bmap_intent;

void xfs_bmap_defer_add(struct xfs_trans *tp, struct xfs_bmap_intent *bi);

#endif /* __LIBXFS_DEFER_ITEM_H_ */
