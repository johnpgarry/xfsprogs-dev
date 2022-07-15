// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"
#include "libfrog/file_exchange.h"

static cmdinfo_t swapext_cmd;

static void
swapext_help(void)
{
	printf(_(
"\n"
" Swaps extents between the open file descriptor and the supplied filename.\n"
"\n"));
}

static int
swapext_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct xfs_exch_range	fxr;
	struct stat		stat;
	uint64_t		flags = XFS_EXCH_RANGE_FILE2_FRESH |
					XFS_EXCH_RANGE_FULL_FILES;
	int			fd;
	int			ret;

	/* open the donor file */
	fd = openfile(argv[1], NULL, 0, 0, NULL);
	if (fd < 0)
		return 0;

	ret = -xfd_prepare_geometry(&xfd);
	if (ret) {
		xfrog_perror(ret, "xfd_prepare_geometry");
		exitcode = 1;
		goto out;
	}

	ret = fstat(file->fd, &stat);
	if (ret) {
		perror("fstat");
		exitcode = 1;
		goto out;
	}

	ret = xfrog_file_exchange_prep(&xfd, flags, 0, fd, 0, stat.st_size,
			&fxr);
	if (ret) {
		xfrog_perror(ret, "xfrog_file_exchange_prep");
		exitcode = 1;
		goto out;
	}

	ret = xfrog_file_exchange(&xfd, &fxr);
	if (ret) {
		xfrog_perror(ret, "swapext");
		exitcode = 1;
		goto out;
	}
out:
	close(fd);
	return 0;
}

void
swapext_init(void)
{
	swapext_cmd.name = "swapext";
	swapext_cmd.cfunc = swapext_f;
	swapext_cmd.argmin = 1;
	swapext_cmd.argmax = 1;
	swapext_cmd.flags = CMD_NOMAP_OK;
	swapext_cmd.args = _("<donorfile>");
	swapext_cmd.oneline = _("Swap extents between files.");
	swapext_cmd.help = swapext_help;

	add_command(&swapext_cmd);
}
