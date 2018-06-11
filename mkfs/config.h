/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * Copyright (c) 2016-2017 Red Hat, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef _XFS_MKFS_CONFIG_H
#define _XFS_MKFS_CONFIG_H

struct fsxattr;

/*
 * Shared superblock configuration options
 *
 * These options provide shared configuration tunables for the filesystem
 * superblock. There are three possible sources for these options set, each
 * source can overriding the later source:
 *
 * 	o built-in defaults
 * 	o configuration file (XXX)
 * 	o command line
 *
 * These values are not used directly - they are inputs into the mkfs geometry
 * validation.
 */
struct sb_feat_args {
	int	log_version;
	int	attr_version;
	int	dir_version;
	bool	inode_align;		/* XFS_SB_VERSION_ALIGNBIT */
	bool	nci;			/* XFS_SB_VERSION_BORGBIT */
	bool	lazy_sb_counters;	/* XFS_SB_VERSION2_LAZYSBCOUNTBIT */
	bool	parent_pointers;	/* XFS_SB_VERSION2_PARENTBIT */
	bool	projid32bit;		/* XFS_SB_VERSION2_PROJID32BIT */
	bool	crcs_enabled;		/* XFS_SB_VERSION2_CRCBIT */
	bool	dirftype;		/* XFS_SB_VERSION2_FTYPE */
	bool	finobt;			/* XFS_SB_FEAT_RO_COMPAT_FINOBT */
	bool	spinodes;		/* XFS_SB_FEAT_INCOMPAT_SPINODES */
	bool	rmapbt;			/* XFS_SB_FEAT_RO_COMPAT_RMAPBT */
	bool	reflink;		/* XFS_SB_FEAT_RO_COMPAT_REFLINK */
	bool	nodalign;
	bool	nortalign;
};

/*
 * Default filesystem features and configuration values
 *
 * This structure contains the default mkfs values that are to be used when
 * a user does not specify the option on the command line. We do not use these
 * values directly - they are inputs to the mkfs geometry validation and
 * calculations.
 */
struct mkfs_default_params {
	char	*source;	/* where the defaults came from */

	int	sectorsize;
	int	blocksize;

	/* feature flags that are set */
	struct sb_feat_args	sb_feat;

	/* root inode characteristics */
	struct fsxattr		fsx;
};

#endif /* _XFS_MKFS_CONFIG_H */
