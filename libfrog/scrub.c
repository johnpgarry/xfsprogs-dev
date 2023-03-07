// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019 Oracle, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "fsgeom.h"
#include "scrub.h"

/* These must correspond to XFS_SCRUB_TYPE_ */
const struct xfrog_scrub_descr xfrog_scrubbers[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_PROBE] = {
		.name	= "probe",
		.descr	= "metadata",
		.group	= XFROG_SCRUB_GROUP_NONE,
	},
	[XFS_SCRUB_TYPE_SB] = {
		.name	= "sb",
		.descr	= "superblock",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGF] = {
		.name	= "agf",
		.descr	= "free space header",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGFL] = {
		.name	= "agfl",
		.descr	= "free list",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGI] = {
		.name	= "agi",
		.descr	= "inode header",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_BNOBT] = {
		.name	= "bnobt",
		.descr	= "freesp by block btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_CNTBT] = {
		.name	= "cntbt",
		.descr	= "freesp by length btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_INOBT] = {
		.name	= "inobt",
		.descr	= "inode btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_FINOBT] = {
		.name	= "finobt",
		.descr	= "free inode btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_RMAPBT] = {
		.name	= "rmapbt",
		.descr	= "reverse mapping btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_REFCNTBT] = {
		.name	= "refcountbt",
		.descr	= "reference count btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_INODE] = {
		.name	= "inode",
		.descr	= "inode record",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTD] = {
		.name	= "bmapbtd",
		.descr	= "data block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTA] = {
		.name	= "bmapbta",
		.descr	= "attr block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTC] = {
		.name	= "bmapbtc",
		.descr	= "CoW block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_DIR] = {
		.name	= "directory",
		.descr	= "directory entries",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_XATTR] = {
		.name	= "xattr",
		.descr	= "extended attributes",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_SYMLINK] = {
		.name	= "symlink",
		.descr	= "symbolic link",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_PARENT] = {
		.name	= "parent",
		.descr	= "parent pointer",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_RTBITMAP] = {
		.name	= "rtbitmap",
		.descr	= "realtime bitmap",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_RTSUM] = {
		.name	= "rtsummary",
		.descr	= "realtime summary",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_UQUOTA] = {
		.name	= "usrquota",
		.descr	= "user quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_GQUOTA] = {
		.name	= "grpquota",
		.descr	= "group quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_PQUOTA] = {
		.name	= "prjquota",
		.descr	= "project quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_FSCOUNTERS] = {
		.name	= "fscounters",
		.descr	= "filesystem summary counters",
		.group	= XFROG_SCRUB_GROUP_SUMMARY,
	},
	[XFS_SCRUB_TYPE_QUOTACHECK] = {
		.name	= "quotacheck",
		.descr	= "quota counters",
		.group	= XFROG_SCRUB_GROUP_ISCAN,
	},
	[XFS_SCRUB_TYPE_NLINKS] = {
		.name	= "nlinks",
		.descr	= "inode link counts",
		.group	= XFROG_SCRUB_GROUP_ISCAN,
	},
	[XFS_SCRUB_TYPE_HEALTHY] = {
		.name	= "healthy",
		.descr	= "retained health records",
		.group	= XFROG_SCRUB_GROUP_NONE,
	},
};

/* Invoke the scrub ioctl.  Returns zero or negative error code. */
int
xfrog_scrub_metadata(
	struct xfs_fd			*xfd,
	struct xfs_scrub_metadata	*meta)
{
	int				ret;

	ret = ioctl(xfd->fd, XFS_IOC_SCRUB_METADATA, meta);
	if (ret)
		return -errno;

	return 0;
}
