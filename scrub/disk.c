/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <linux/fs.h>
#include "platform_defs.h"
#include "libfrog.h"
#include "xfs.h"
#include "path.h"
#include "xfs_fs.h"
#include "xfs_scrub.h"
#include "disk.h"

#ifndef BLKROTATIONAL
# define BLKROTATIONAL	_IO(0x12, 126)
#endif

/*
 * Disk Abstraction
 *
 * These routines help us to discover the geometry of a block device,
 * estimate the amount of concurrent IOs that we can send to it, and
 * abstract the process of performing read verification of disk blocks.
 */

/* Figure out how many disk heads are available. */
static unsigned int
__disk_heads(
	struct disk		*disk)
{
	int			iomin;
	int			ioopt;
	unsigned short		rot;
	int			error;

	/* If it's not a block device, throw all the CPUs at it. */
	if (!S_ISBLK(disk->d_sb.st_mode))
		return nproc;

	/* Non-rotational device?  Throw all the CPUs at the problem. */
	rot = 1;
	error = ioctl(disk->d_fd, BLKROTATIONAL, &rot);
	if (error == 0 && rot == 0)
		return nproc;

	/*
	 * Sometimes we can infer the number of devices from the
	 * min/optimal IO sizes.
	 */
	iomin = ioopt = 0;
	if (ioctl(disk->d_fd, BLKIOMIN, &iomin) == 0 &&
	    ioctl(disk->d_fd, BLKIOOPT, &ioopt) == 0 &&
	    iomin > 0 && ioopt > 0) {
		return min(nproc, max(1, ioopt / iomin));
	}

	/* Rotating device?  I guess? */
	return 2;
}

/* Figure out how many disk heads are available. */
unsigned int
disk_heads(
	struct disk		*disk)
{
	if (nr_threads)
		return nr_threads;
	return __disk_heads(disk);
}

/* Open a disk device and discover its geometry. */
struct disk *
disk_open(
	const char		*pathname)
{
	struct disk		*disk;
	int			lba_sz;
	int			error;

	disk = calloc(1, sizeof(struct disk));
	if (!disk)
		return NULL;

	disk->d_fd = open(pathname, O_RDONLY | O_DIRECT | O_NOATIME);
	if (disk->d_fd < 0)
		goto out_free;

	/* Try to get LBA size. */
	error = ioctl(disk->d_fd, BLKSSZGET, &lba_sz);
	if (error)
		lba_sz = 512;
	disk->d_lbalog = log2_roundup(lba_sz);

	/* Obtain disk's stat info. */
	error = fstat(disk->d_fd, &disk->d_sb);
	if (error)
		goto out_close;

	/* Determine bdev size, block size, and offset. */
	if (S_ISBLK(disk->d_sb.st_mode)) {
		error = ioctl(disk->d_fd, BLKGETSIZE64, &disk->d_size);
		if (error)
			disk->d_size = 0;
		error = ioctl(disk->d_fd, BLKBSZGET, &disk->d_blksize);
		if (error)
			disk->d_blksize = 0;
		disk->d_start = 0;
	} else {
		disk->d_size = disk->d_sb.st_size;
		disk->d_blksize = disk->d_sb.st_blksize;
		disk->d_start = 0;
	}

	return disk;
out_close:
	close(disk->d_fd);
out_free:
	free(disk);
	return NULL;
}

/* Close a disk device. */
int
disk_close(
	struct disk		*disk)
{
	int			error = 0;

	if (disk->d_fd >= 0)
		error = close(disk->d_fd);
	disk->d_fd = -1;
	free(disk);
	return error;
}

/* Read-verify an extent of a disk device. */
ssize_t
disk_read_verify(
	struct disk		*disk,
	void			*buf,
	uint64_t		start,
	uint64_t		length)
{
	return pread(disk->d_fd, buf, length, start);
}
