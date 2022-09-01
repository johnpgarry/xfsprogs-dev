// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2024 Oracle.  All rights reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "libxfs.h"
#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/paths.h"
#include "libfrog/fsgeom.h"

static cmdinfo_t aginfo_cmd;

static int
report_aginfo(
	struct xfs_fd		*xfd,
	xfs_agnumber_t		agno)
{
	struct xfs_ag_geometry	ageo = { 0 };
	int			ret;

	ret = -xfrog_ag_geometry(xfd->fd, agno, &ageo);
	if (ret) {
		xfrog_perror(ret, "aginfo");
		return 1;
	}

	printf(_("AG: %u\n"),		ageo.ag_number);
	printf(_("Blocks: %u\n"),	ageo.ag_length);
	printf(_("Free Blocks: %u\n"),	ageo.ag_freeblks);
	printf(_("Inodes: %u\n"),	ageo.ag_icount);
	printf(_("Free Inodes: %u\n"),	ageo.ag_ifree);
	printf(_("Sick: 0x%x\n"),	ageo.ag_sick);
	printf(_("Checked: 0x%x\n"),	ageo.ag_checked);
	printf(_("Flags: 0x%x\n"),	ageo.ag_flags);

	return 0;
}

/* Display AG status. */
static int
aginfo_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	unsigned long long	x;
	xfs_agnumber_t		agno = NULLAGNUMBER;
	int			c;
	int			ret = 0;

	ret = -xfd_prepare_geometry(&xfd);
	if (ret) {
		xfrog_perror(ret, "xfd_prepare_geometry");
		exitcode = 1;
		return 1;
	}

	while ((c = getopt(argc, argv, "a:")) != EOF) {
		switch (c) {
		case 'a':
			errno = 0;
			x = strtoll(optarg, NULL, 10);
			if (!errno && x >= NULLAGNUMBER)
				errno = ERANGE;
			if (errno) {
				perror("aginfo");
				return 1;
			}
			agno = x;
			break;
		default:
			return command_usage(&aginfo_cmd);
		}
	}

	if (agno != NULLAGNUMBER) {
		ret = report_aginfo(&xfd, agno);
	} else {
		for (agno = 0; !ret && agno < xfd.fsgeom.agcount; agno++) {
			ret = report_aginfo(&xfd, agno);
		}
	}

	return ret;
}

static void
aginfo_help(void)
{
	printf(_(
"\n"
"Report allocation group geometry.\n"
"\n"
" -a agno  -- Report on the given allocation group.\n"
"\n"));

}

static cmdinfo_t aginfo_cmd = {
	.name = "aginfo",
	.cfunc = aginfo_f,
	.argmin = 0,
	.argmax = -1,
	.args = "[-a agno]",
	.flags = CMD_NOMAP_OK,
	.help = aginfo_help,
};

void
aginfo_init(void)
{
	aginfo_cmd.oneline = _("Get XFS allocation group state.");
	add_command(&aginfo_cmd);
}
