// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include "xfrog.h"

/* Bulkstat a single inode. */
int
xfrog_bulkstat_single(
	struct xfs_fd		*xfd,
	uint64_t		ino,
	struct xfs_bstat	*ubuffer)
{
	__u64			i = ino;
	struct xfs_fsop_bulkreq	bulkreq = {
		.lastip		= &i,
		.icount		= 1,
		.ubuffer	= ubuffer,
		.ocount		= NULL,
	};

	return ioctl(xfd->fd, XFS_IOC_FSBULKSTAT_SINGLE, &bulkreq);
}

/* Bulkstat a bunch of inodes. */
int
xfrog_bulkstat(
	struct xfs_fd		*xfd,
	uint64_t		*lastino,
	uint32_t		icount,
	struct xfs_bstat	*ubuffer,
	uint32_t		*ocount)
{
	struct xfs_fsop_bulkreq	bulkreq = {
		.lastip		= (__u64 *)lastino,
		.icount		= icount,
		.ubuffer	= ubuffer,
		.ocount		= (__s32 *)ocount,
	};

	return ioctl(xfd->fd, XFS_IOC_FSBULKSTAT, &bulkreq);
}
