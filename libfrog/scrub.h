/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2019 Oracle, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_SCRUB_H__
#define __LIBFROG_SCRUB_H__

/* Group the scrub types by principal filesystem object. */
enum xfrog_scrub_group {
	XFROG_SCRUB_GROUP_NONE,		/* not metadata */
	XFROG_SCRUB_GROUP_AGHEADER,	/* per-AG header */
	XFROG_SCRUB_GROUP_PERAG,	/* per-AG metadata */
	XFROG_SCRUB_GROUP_FS,		/* per-FS metadata */
	XFROG_SCRUB_GROUP_INODE,	/* per-inode metadata */
	XFROG_SCRUB_GROUP_ISCAN,	/* metadata requiring full inode scan */
	XFROG_SCRUB_GROUP_SUMMARY,	/* summary metadata */
};

/* Catalog of scrub types and names, indexed by XFS_SCRUB_TYPE_* */
struct xfrog_scrub_descr {
	const char		*name;
	const char		*descr;
	enum xfrog_scrub_group	group;
};

extern const struct xfrog_scrub_descr xfrog_scrubbers[XFS_SCRUB_TYPE_NR];

int xfrog_scrub_metadata(struct xfs_fd *xfd, struct xfs_scrub_metadata *meta);

#endif	/* __LIBFROG_SCRUB_H__ */
