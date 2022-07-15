// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include "xfs.h"
#include "fsgeom.h"
#include "bulkstat.h"
#include "file_exchange.h"

/* Prepare the freshness component of a swapext request. */
static int
xfrog_file_exchange_prep_freshness(
	struct xfs_fd		*dest,
	struct xfs_exch_range	*req)
{
	struct stat		stat;
	struct xfs_bulkstat	bulkstat;
	int			error;

	error = fstat(dest->fd, &stat);
	if (error)
		return -errno;
	req->file2_ino = stat.st_ino;

	/*
	 * Try to fill out the [cm]time data from bulkstat.  We prefer this
	 * approach because bulkstat v5 gives us 64-bit time even on 32-bit.
	 *
	 * However, we'll take our chances on the C library if the filesystem
	 * supports 64-bit time but we ended up with bulkstat v5 emulation.
	 */
	error = xfrog_bulkstat_single(dest, stat.st_ino, 0, &bulkstat);
	if (!error &&
	    !((dest->fsgeom.flags & XFS_FSOP_GEOM_FLAGS_BIGTIME) &&
	      bulkstat.bs_version < XFS_BULKSTAT_VERSION_V5)) {
		req->file2_mtime = bulkstat.bs_mtime;
		req->file2_ctime = bulkstat.bs_ctime;
		req->file2_mtime_nsec = bulkstat.bs_mtime_nsec;
		req->file2_ctime_nsec = bulkstat.bs_ctime_nsec;
		return 0;
	}

	/* Otherwise, use the stat information and hope for the best. */
	req->file2_mtime = stat.st_mtime;
	req->file2_ctime = stat.st_ctime;
	req->file2_mtime_nsec = stat.st_mtim.tv_nsec;
	req->file2_ctime_nsec = stat.st_ctim.tv_nsec;
	return 0;
}

/* Prepare an extent swap request. */
int
xfrog_file_exchange_prep(
	struct xfs_fd		*dest,
	uint64_t		flags,
	int64_t			file2_offset,
	int			file1_fd,
	int64_t			file1_offset,
	int64_t			length,
	struct xfs_exch_range	*req)
{
	memset(req, 0, sizeof(*req));
	req->file1_fd = file1_fd;
	req->file1_offset = file1_offset;
	req->length = length;
	req->file2_offset = file2_offset;
	req->flags = flags;

	if (flags & XFS_EXCH_RANGE_FILE2_FRESH)
		return xfrog_file_exchange_prep_freshness(dest, req);

	return 0;
}

/* Swap two files' extents with the new exchange range ioctl. */
static int
xfrog_file_exchange_range(
	struct xfs_fd		*xfd,
	struct xfs_exch_range	*req)
{
	int			ret;

	ret = ioctl(xfd->fd, XFS_IOC_EXCHANGE_RANGE, req);
	if (ret) {
		/* the old swapext ioctl returned EFAULT for bad length */
		if (errno == EDOM)
			return -EFAULT;
		return -errno;
	}
	return 0;
}

/*
 * The old swapext ioctl did not provide atomic swap; it required that the
 * supplied offset and length matched both files' lengths; and it also required
 * that the sx_stat information match the dest file.  It doesn't support any
 * other flags.
 */
#define XFS_EXCH_RANGE_SWAPEXT	(XFS_EXCH_RANGE_NONATOMIC | \
				 XFS_EXCH_RANGE_FULL_FILES | \
				 XFS_EXCH_RANGE_FILE2_FRESH)

/* Swap two files' extents with the old xfs swapext ioctl. */
static int
xfrog_file_exchange_swapext(
	struct xfs_fd		*xfd,
	struct xfs_exch_range	*req)
{
	struct xfs_swapext	sx = {
		.sx_version	= XFS_SX_VERSION,
		.sx_fdtarget	= xfd->fd,
		.sx_fdtmp	= req->file1_fd,
		.sx_length	= req->length,
	};
	int			ret;

	if (req->file1_offset != req->file2_offset)
		return -EINVAL;
	if (req->flags != XFS_EXCH_RANGE_SWAPEXT)
		return -EOPNOTSUPP;

	sx.sx_stat.bs_ino = req->file2_ino;
	sx.sx_stat.bs_ctime.tv_sec = req->file2_ctime;
	sx.sx_stat.bs_ctime.tv_nsec = req->file2_ctime_nsec;
	sx.sx_stat.bs_mtime.tv_sec = req->file2_mtime;
	sx.sx_stat.bs_mtime.tv_nsec = req->file2_mtime_nsec;

	ret = ioctl(xfd->fd, XFS_IOC_SWAPEXT, &sx);
	if (ret)
		return -errno;
	return 0;
}

/* Swap extents between an XFS file and a donor fd. */
int
xfrog_file_exchange(
	struct xfs_fd		*xfd,
	struct xfs_exch_range	*req)
{
	int			error;

	if (xfd->flags & XFROG_FLAG_FORCE_SWAPEXT)
		goto try_swapext;

	error = xfrog_file_exchange_range(xfd, req);
	if ((error != -ENOTTY && error != -EOPNOTSUPP) ||
	    (xfd->flags & XFROG_FLAG_FORCE_EXCH_RANGE))
		return error;

	/*
	 * If the new exchange range ioctl wasn't found, punt to the old
	 * swapext ioctl.
	 */
	switch (error) {
	case -EOPNOTSUPP:
	case -ENOTTY:
		xfd->flags |= XFROG_FLAG_FORCE_SWAPEXT;
		break;
	}

try_swapext:
	return xfrog_file_exchange_swapext(xfd, req);
}
