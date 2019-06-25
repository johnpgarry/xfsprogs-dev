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

	/* log2 of sb_blocksize / sb_sectsize */
	unsigned int		blkbb_log;
};

/* Static initializers */
#define XFS_FD_INIT(_fd)	{ .fd = (_fd), }
#define XFS_FD_INIT_EMPTY	XFS_FD_INIT(-1)

int xfrog_prepare_geometry(struct xfs_fd *xfd);
int xfrog_close(struct xfs_fd *xfd);

/* Convert AG number and AG inode number into fs inode number. */
static inline uint64_t
xfrog_agino_to_ino(
	struct xfs_fd		*xfd,
	uint32_t		agno,
	uint32_t		agino)
{
	return ((uint64_t)agno << xfd->aginolog) + agino;
}

/* Convert fs inode number into AG number. */
static inline uint32_t
xfrog_ino_to_agno(
	struct xfs_fd		*xfd,
	uint64_t		ino)
{
	return ino >> xfd->aginolog;
}

/* Convert fs inode number into AG inode number. */
static inline uint32_t
xfrog_ino_to_agino(
	struct xfs_fd		*xfd,
	uint64_t		ino)
{
	return ino & ((1ULL << xfd->aginolog) - 1);
}

/* Convert fs block number into bytes */
static inline uint64_t
xfrog_fsb_to_b(
	struct xfs_fd		*xfd,
	uint64_t		fsb)
{
	return fsb << xfd->blocklog;
}

/* Convert bytes into (rounded down) fs block number */
static inline uint64_t
xfrog_b_to_fsbt(
	struct xfs_fd		*xfd,
	uint64_t		bytes)
{
	return bytes >> xfd->blocklog;
}

/* Convert sector number to bytes. */
static inline uint64_t
xfrog_bbtob(
	uint64_t		daddr)
{
	return daddr << BBSHIFT;
}

/* Convert bytes to sector number, rounding down. */
static inline uint64_t
xfrog_btobbt(
	uint64_t		bytes)
{
	return bytes >> BBSHIFT;
}

/* Convert fs block number to sector number. */
static inline uint64_t
xfrog_fsb_to_bb(
	struct xfs_fd		*xfd,
	uint64_t		fsbno)
{
	return fsbno << xfd->blkbb_log;
}

/* Convert sector number to fs block number, rounded down. */
static inline uint64_t
xfrog_bb_to_fsbt(
	struct xfs_fd		*xfd,
	uint64_t		daddr)
{
	return daddr >> xfd->blkbb_log;
}

/* Convert AG number and AG block to fs block number */
static inline uint64_t
xfrog_agb_to_daddr(
	struct xfs_fd		*xfd,
	uint32_t		agno,
	uint32_t		agbno)
{
	return xfrog_fsb_to_bb(xfd,
			(uint64_t)agno * xfd->fsgeom.agblocks + agbno);
}

/* Convert sector number to AG number. */
static inline uint32_t
xfrog_daddr_to_agno(
	struct xfs_fd		*xfd,
	uint64_t		daddr)
{
	return xfrog_bb_to_fsbt(xfd, daddr) / xfd->fsgeom.agblocks;
}

/* Convert sector number to AG block number. */
static inline uint32_t
xfrog_daddr_to_agbno(
	struct xfs_fd		*xfd,
	uint64_t		daddr)
{
	return xfrog_bb_to_fsbt(xfd, daddr) % xfd->fsgeom.agblocks;
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

int xfrog_ag_geometry(int fd, unsigned int agno, struct xfs_ag_geometry *ageo);

#endif	/* __XFROG_H__ */
