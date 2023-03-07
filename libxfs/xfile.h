/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBXFS_XFILE_H__
#define __LIBXFS_XFILE_H__

struct xfile {
	int		fd;
};

void xfile_libinit(void);

int xfile_create(const char *description, struct xfile **xfilep);
void xfile_destroy(struct xfile *xf);

ssize_t xfile_pread(struct xfile *xf, void *buf, size_t count, loff_t pos);
ssize_t xfile_pwrite(struct xfile *xf, const void *buf, size_t count, loff_t pos);

/*
 * Load an object.  Since we're treating this file as "memory", any error or
 * short IO is treated as a failure to allocate memory.
 */
static inline int
xfile_obj_load(struct xfile *xf, void *buf, size_t count, loff_t pos)
{
	ssize_t	ret = xfile_pread(xf, buf, count, pos);

	if (ret < 0 || ret != count)
		return -ENOMEM;
	return 0;
}

/*
 * Store an object.  Since we're treating this file as "memory", any error or
 * short IO is treated as a failure to allocate memory.
 */
static inline int
xfile_obj_store(struct xfile *xf, const void *buf, size_t count, loff_t pos)
{
	ssize_t	ret = xfile_pwrite(xf, buf, count, pos);

	if (ret < 0 || ret != count)
		return -ENOMEM;
	return 0;
}

struct xfile_stat {
	loff_t			size;
	unsigned long long	bytes;
};

int xfile_stat(struct xfile *xf, struct xfile_stat *statbuf);
unsigned long long xfile_bytes(struct xfile *xf);
int xfile_dump(struct xfile *xf);

static inline loff_t xfile_size(struct xfile *xf)
{
	struct xfile_stat	xs;
	int			ret;

	ret = xfile_stat(xf, &xs);
	if (ret)
		return 0;

	return xs.size;
}

/* file block (aka system page size) to basic block conversions. */
typedef unsigned long long	xfileoff_t;
extern unsigned int		XFB_BLOCKSIZE;
extern unsigned int		XFB_BSHIFT;
#define XFB_SHIFT		(XFB_BSHIFT - BBSHIFT)

static inline loff_t xfo_to_b(xfileoff_t xfoff)
{
	return xfoff << XFB_BSHIFT;
}

static inline xfileoff_t b_to_xfo(loff_t pos)
{
	return (pos + (XFB_BLOCKSIZE - 1)) >> XFB_BSHIFT;
}

static inline xfileoff_t b_to_xfot(loff_t pos)
{
	return pos >> XFB_BSHIFT;
}

static inline xfs_daddr_t xfo_to_daddr(xfileoff_t xfoff)
{
	return xfoff << XFB_SHIFT;
}

static inline xfileoff_t xfs_daddr_to_xfo(xfs_daddr_t bb)
{
	return (bb + (xfo_to_daddr(1) - 1)) >> XFB_SHIFT;
}

static inline xfileoff_t xfs_daddr_to_xfot(xfs_daddr_t bb)
{
	return bb >> XFB_SHIFT;
}

#endif /* __LIBXFS_XFILE_H__ */
