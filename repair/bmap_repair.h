/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2019-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef REBUILD_H_
#define REBUILD_H_

int rebuild_bmap(struct xfs_mount *mp, xfs_ino_t ino, int whichfork,
		 unsigned long nr_extents, struct xfs_buf **ino_bpp,
		 struct xfs_dinode **dinop, int *dirty);

#endif /* REBUILD_H_ */
