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

	/* bits for agino in inum */
	unsigned int		aginolog;
};

/* Static initializers */
#define XFS_FD_INIT(_fd)	{ .fd = (_fd), }
#define XFS_FD_INIT_EMPTY	XFS_FD_INIT(-1)

int xfrog_prepare_geometry(struct xfs_fd *xfd);
int xfrog_close(struct xfs_fd *xfd);

/* Convert AG number and AG inode number into fs inode number. */
static inline uint64_t
xfrog_agino_to_ino(
	const struct xfs_fd	*xfd,
	uint32_t		agno,
	uint32_t		agino)
{
	return ((uint64_t)agno << xfd->aginolog) + agino;
}

/* Convert fs inode number into AG number. */
static inline uint32_t
xfrog_ino_to_agno(
	const struct xfs_fd	*xfd,
	uint64_t		ino)
{
	return ino >> xfd->aginolog;
}

/* Convert fs inode number into AG inode number. */
static inline uint32_t
xfrog_ino_to_agino(
	const struct xfs_fd	*xfd,
	uint64_t		ino)
{
	return ino & ((1ULL << xfd->aginolog) - 1);
}

/* Convert fs block number into bytes */
static inline uint64_t
xfrog_fsb_to_b(
	const struct xfs_fd	*xfd,
	uint64_t		fsb)
{
	return fsb << xfd->blocklog;
}

/* Convert bytes into (rounded down) fs block number */
static inline uint64_t
xfrog_b_to_fsbt(
	const struct xfs_fd	*xfd,
	uint64_t		bytes)
{
	return bytes >> xfd->blocklog;
}

/* Bulkstat wrappers */
struct xfs_bstat;
int xfrog_bulkstat_single(struct xfs_fd *xfd, uint64_t ino,
		struct xfs_bstat *ubuffer);
int xfrog_bulkstat(struct xfs_fd *xfd, uint64_t *lastino, uint32_t icount,
		struct xfs_bstat *ubuffer, uint32_t *ocount);

struct xfs_inogrp;
int xfrog_inumbers(struct xfs_fd *xfd, uint64_t *lastino, uint32_t icount,
		struct xfs_inogrp *ubuffer, uint32_t *ocount);

#endif	/* __XFROG_H__ */
