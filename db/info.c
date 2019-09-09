// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "command.h"
#include "init.h"
#include "output.h"
#include "libfrog/fsgeom.h"

static void
info_help(void)
{
	dbprintf(_(
"\n"
" Pretty-prints the filesystem geometry as derived from the superblock.\n"
" The output has the same format as mkfs.xfs, xfs_info, and other utilities.\n"
"\n"
));

}

static int
info_f(
	int			argc,
	char			**argv)
{
	struct xfs_fsop_geom	geo;

	libxfs_fs_geometry(&mp->m_sb, &geo, XFS_FS_GEOM_MAX_STRUCT_VER);
	xfs_report_geom(&geo, fsdevice, x.logname, x.rtname);
	return 0;
}

static const struct cmdinfo info_cmd = {
	.name =		"info",
	.altname =	"i",
	.cfunc =	info_f,
	.argmin =	0,
	.argmax =	0,
	.canpush =	0,
	.args =		NULL,
	.oneline =	N_("pretty-print superblock info"),
	.help =		info_help,
};

void
info_init(void)
{
	add_command(&info_cmd);
}
