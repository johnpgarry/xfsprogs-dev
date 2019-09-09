/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2019 Oracle, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_BULKSTAT_H__
#define __LIBFROG_BULKSTAT_H__

/* Bulkstat wrappers */
struct xfs_bstat;
int xfrog_bulkstat_single(struct xfs_fd *xfd, uint64_t ino,
		struct xfs_bstat *ubuffer);
int xfrog_bulkstat(struct xfs_fd *xfd, uint64_t *lastino, uint32_t icount,
		struct xfs_bstat *ubuffer, uint32_t *ocount);

#endif	/* __LIBFROG_BULKSTAT_H__ */
