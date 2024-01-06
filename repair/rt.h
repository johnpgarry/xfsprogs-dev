// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef _XFS_REPAIR_RT_H_
#define _XFS_REPAIR_RT_H_

struct blkmap;

void
rtinit(xfs_mount_t		*mp);

int generate_rtinfo(struct xfs_mount *mp, union xfs_rtword_raw *words,
		union xfs_suminfo_raw *sumcompute);

void check_rtbitmap(struct xfs_mount *mp);
void check_rtsummary(struct xfs_mount *mp);

#endif /* _XFS_REPAIR_RT_H_ */
