// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
 */
#ifndef _LIBFROG_FSGEOM_H_
#define _LIBFROG_FSGEOM_H_

void xfs_report_geom(struct xfs_fsop_geom *geo, const char *mntpoint,
		const char *logname, const char *rtname);
int xfrog_geometry(int fd, struct xfs_fsop_geom *fsgeo);

#endif /* _LIBFROG_FSGEOM_H_ */
