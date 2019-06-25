// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFROG_H__
#define __XFROG_H__

/*
 * XFS Filesystem Random Online Gluecode
 * =====================================
 *
 * These support functions wrap the more complex xfs ioctls so that xfs
 * utilities can take advantage of them without having to deal with graceful
 * degradation in the face of new ioctls.  They will also provide higher level
 * abstractions when possible.
 */

struct xfs_fsop_geom;
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

int xfrog_prepare_geometry(struct xfs_fd *xfd);
int xfrog_close(struct xfs_fd *xfd);

#endif	/* __XFROG_H__ */
