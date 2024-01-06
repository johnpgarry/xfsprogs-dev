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

#endif /* __LIBXFS_XFILE_H__ */
