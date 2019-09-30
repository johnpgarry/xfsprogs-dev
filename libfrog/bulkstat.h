/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2019 Oracle, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_BULKSTAT_H__
#define __LIBFROG_BULKSTAT_H__

/* Bulkstat wrappers */
struct xfs_bstat;
int xfrog_bulkstat_single(struct xfs_fd *xfd, uint64_t ino, unsigned int flags,
		struct xfs_bulkstat *bulkstat);
int xfrog_bulkstat(struct xfs_fd *xfd, struct xfs_bulkstat_req *req);

struct xfs_bulkstat_req *xfrog_bulkstat_alloc_req(uint32_t nr,
		uint64_t startino);
int xfrog_bulkstat_v5_to_v1(struct xfs_fd *xfd, struct xfs_bstat *bs1,
		const struct xfs_bulkstat *bstat);
void xfrog_bulkstat_v1_to_v5(struct xfs_fd *xfd, struct xfs_bulkstat *bstat,
		const struct xfs_bstat *bs1);

struct xfs_inogrp;
int xfrog_inumbers(struct xfs_fd *xfd, uint64_t *lastino, uint32_t icount,
		struct xfs_inogrp *ubuffer, uint32_t *ocount);

#endif	/* __LIBFROG_BULKSTAT_H__ */
