// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef __LIBXFS_TOPOLOGY_H__
#define __LIBXFS_TOPOLOGY_H__

/*
 * Device topology information.
 */
typedef struct fs_topology {
	int	dsunit;		/* stripe unit - data subvolume */
	int	dswidth;	/* stripe width - data subvolume */
	int	rtswidth;	/* stripe width - rt subvolume */
	int	lsectorsize;	/* logical sector size &*/
	int	psectorsize;	/* physical sector size */
} fs_topology_t;

void
get_topology(
	struct libxfs_init	*xi,
	struct fs_topology	*ft,
	int			force_overwrite);

extern void
calc_default_ag_geometry(
	int		blocklog,
	uint64_t	dblocks,
	int		multidisk,
	uint64_t	*agsize,
	uint64_t	*agcount);

void calc_default_rtgroup_geometry(int blocklog, uint64_t rblocks,
		uint64_t *rgsize, uint64_t *rgcount);

extern int
check_overwrite(
	const char	*device);

#endif	/* __LIBXFS_TOPOLOGY_H__ */
