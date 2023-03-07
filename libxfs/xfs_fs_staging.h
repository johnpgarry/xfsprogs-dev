/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_FS_STAGING_H__
#define __XFS_FS_STAGING_H__

/*
 * Experimental system calls, ioctls and data structures supporting them.
 * Nothing in here should be considered part of a stable interface of any kind.
 *
 * If you add an ioctl here, please leave a comment in xfs_fs.h marking it
 * reserved.  If you promote anything out of this file, please leave a comment
 * explaining where it went.
 */

/*
 * Exchange part of file1 with part of the file that this ioctl that is being
 * called against (which we'll call file2).  Filesystems must be able to
 * restart and complete the operation even after the system goes down.
 */
struct xfs_exch_range {
	__s64		file1_fd;
	__s64		file1_offset;	/* file1 offset, bytes */
	__s64		file2_offset;	/* file2 offset, bytes */
	__u64		length;		/* bytes to exchange */

	__u64		flags;		/* see XFS_EXCH_RANGE_* below */

	/* file2 metadata for optional freshness checks */
	__s64		file2_ino;	/* inode number */
	__s64		file2_mtime;	/* modification time */
	__s64		file2_ctime;	/* change time */
	__s32		file2_mtime_nsec; /* mod time, nsec */
	__s32		file2_ctime_nsec; /* change time, nsec */

	__u64		pad[6];		/* must be zeroes */
};

/*
 * Atomic exchange operations are not required.  This relaxes the requirement
 * that the filesystem must be able to complete the operation after a crash.
 */
#define XFS_EXCH_RANGE_NONATOMIC	(1 << 0)

/*
 * Check that file2's inode number, mtime, and ctime against the values
 * provided, and return -EBUSY if there isn't an exact match.
 */
#define XFS_EXCH_RANGE_FILE2_FRESH	(1 << 1)

/*
 * Check that the file1's length is equal to file1_offset + length, and that
 * file2's length is equal to file2_offset + length.  Returns -EDOM if there
 * isn't an exact match.
 */
#define XFS_EXCH_RANGE_FULL_FILES	(1 << 2)

/*
 * Exchange file data all the way to the ends of both files, and then exchange
 * the file sizes.  This flag can be used to replace a file's contents with a
 * different amount of data.  length will be ignored.
 */
#define XFS_EXCH_RANGE_TO_EOF		(1 << 3)

/* Flush all changes in file data and file metadata to disk before returning. */
#define XFS_EXCH_RANGE_FSYNC		(1 << 4)

/* Dry run; do all the parameter verification but do not change anything. */
#define XFS_EXCH_RANGE_DRY_RUN		(1 << 5)

/*
 * Exchange only the parts of the two files where the file allocation units
 * mapped to file1's range have been written to.  This can accelerate
 * scatter-gather atomic writes with a temp file if all writes are aligned to
 * the file allocation unit.
 */
#define XFS_EXCH_RANGE_FILE1_WRITTEN	(1 << 6)

/*
 * Commit the contents of file1 into file2 if file2 has the same inode number,
 * mtime, and ctime as the arguments provided to the call.  The old contents of
 * file2 will be moved to file1.
 *
 * With this flag, all committed information can be retrieved even if the
 * system crashes or is rebooted.  This includes writing through or flushing a
 * disk cache if present.  The call blocks until the device reports that the
 * commit is complete.
 *
 * This flag should not be combined with NONATOMIC.  It can be combined with
 * FILE1_WRITTEN.
 */
#define XFS_EXCH_RANGE_COMMIT		(XFS_EXCH_RANGE_FILE2_FRESH | \
					 XFS_EXCH_RANGE_FSYNC)

#define XFS_EXCH_RANGE_ALL_FLAGS	(XFS_EXCH_RANGE_NONATOMIC | \
					 XFS_EXCH_RANGE_FILE2_FRESH | \
					 XFS_EXCH_RANGE_FULL_FILES | \
					 XFS_EXCH_RANGE_TO_EOF | \
					 XFS_EXCH_RANGE_FSYNC | \
					 XFS_EXCH_RANGE_DRY_RUN | \
					 XFS_EXCH_RANGE_FILE1_WRITTEN)

#define XFS_IOC_EXCHANGE_RANGE	_IOWR('X', 129, struct xfs_exch_range)

#endif /* __XFS_FS_STAGING_H__ */
