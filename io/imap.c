// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2001-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "xfrog.h"

static cmdinfo_t imap_cmd;

static int
imap_f(int argc, char **argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct xfs_inogrp	*t;
	uint64_t		last = 0;
	uint32_t		count;
	uint32_t		nent;
	int			i;
	int			error;

	if (argc != 2)
		nent = 1;
	else
		nent = atoi(argv[1]);

	t = malloc(nent * sizeof(*t));
	if (!t)
		return 0;

	while ((error = xfrog_inumbers(&xfd, &last, nent, t, &count)) == 0 &&
	       count > 0) {
		for (i = 0; i < count; i++) {
			printf(_("ino %10llu count %2d mask %016llx\n"),
				(unsigned long long)t[i].xi_startino,
				t[i].xi_alloccount,
				(unsigned long long)t[i].xi_allocmask);
		}
	}

	if (error) {
		perror("xfsctl(XFS_IOC_FSINUMBERS)");
		exitcode = 1;
	}
	free(t);
	return 0;
}

void
imap_init(void)
{
	imap_cmd.name = "imap";
	imap_cmd.cfunc = imap_f;
	imap_cmd.argmin = 0;
	imap_cmd.argmax = 1;
	imap_cmd.args = _("[nentries]");
	imap_cmd.flags = CMD_NOMAP_OK | CMD_FLAG_ONESHOT;
	imap_cmd.oneline = _("inode map for filesystem of current file");

	if (expert)
		add_command(&imap_cmd);
}
