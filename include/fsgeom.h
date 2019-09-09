// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
 */
#ifndef _LIBFROG_FSGEOM_H_
#define _LIBFROG_FSGEOM_H_

void xfs_report_geom(struct xfs_fsop_geom *geo, const char *mntpoint,
		const char *logname, const char *rtname);
int xfrog_geometry(int fd, struct xfs_fsop_geom *fsgeo);

/*
 * Structure for recording whatever observations we want about the level of
 * xfs runtime support for this fd.  Right now we only store the fd and fs
 * geometry.
 */
struct xfs_fd {
	/* ioctl file descriptor */
	int			fd;

	/* filesystem geometry */
	struct xfs_fsop_geom	fsgeom;

	/* log2 of sb_agblocks (rounded up) */
	unsigned int		agblklog;

	/* log2 of sb_blocksize */
	unsigned int		blocklog;

	/* log2 of sb_inodesize */
	unsigned int		inodelog;

	/* log2 of sb_inopblock */
	unsigned int		inopblog;
};

/* Static initializers */
#define XFS_FD_INIT(_fd)	{ .fd = (_fd), }
#define XFS_FD_INIT_EMPTY	XFS_FD_INIT(-1)

int xfd_prepare_geometry(struct xfs_fd *xfd);
int xfd_close(struct xfs_fd *xfd);

#endif /* _LIBFROG_FSGEOM_H_ */
