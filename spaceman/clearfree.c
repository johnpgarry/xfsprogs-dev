// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "input.h"
#include "libfrog/fsgeom.h"
#include "libfrog/clearspace.h"
#include "handle.h"
#include "space.h"

static void
clearfree_help(void)
{
	printf(_(
"Evacuate the contents of the given range of physical storage in the filesystem"
"\n"
" -n -- Run the space clearing algorithm this many times.\n"
" -r -- clear space on the realtime device.\n"
" -v -- verbosity level, or \"all\" to print everything.\n"
"\n"
"The start and length arguments are required, and must be specified in units\n"
"of bytes.\n"
"\n"));
}

static int
clearfree_f(
	int			argc,
	char			**argv)
{
	struct clearspace_init	attrs = {
		.xfd		= &file->xfd,
		.fname		= file->name,
	};
	struct clearspace_req	*req = NULL;
	unsigned long long	cleared;
	unsigned long		arg;
	long long		lnum;
	unsigned int		i, nr = 1;
	int			c, ret;

	while ((c = getopt(argc, argv, "n:rv:")) != EOF) {
		switch (c) {
		case 'n':
			errno = 0;
			arg = strtoul(optarg, NULL, 0);
			if (errno) {
				perror(optarg);
				return 1;
			}
			if (arg > UINT_MAX)
				arg = UINT_MAX;
			nr = arg;
			break;
		case 'r':	/* rt device */
			attrs.is_realtime = true;
			break;
		case 'v':	/* Verbose output */
			if (!strcmp(optarg, "all")) {
				attrs.trace_mask = CSP_TRACE_ALL;
			} else {
				errno = 0;
				attrs.trace_mask = strtoul(optarg, NULL, 0);
				if (errno) {
					perror(optarg);
					return 1;
				}
			}
			break;
		default:
			exitcode = 1;
			clearfree_help();
			return 0;
		}
	}

	if (attrs.trace_mask)
		attrs.trace_mask |= CSP_TRACE_STATUS;

	if (argc != optind + 2) {
		clearfree_help();
		goto fail;
	}

	if (attrs.is_realtime) {
		if (file->xfd.fsgeom.rtblocks == 0) {
			fprintf(stderr, _("No realtime volume present.\n"));
			goto fail;
		}
		attrs.dev = file->fs_path.fs_rtdev;
	} else {
		attrs.dev = file->fs_path.fs_datadev;
	}

	lnum = cvtnum(file->xfd.fsgeom.blocksize, file->xfd.fsgeom.sectsize,
			argv[optind]);
	if (lnum < 0) {
		fprintf(stderr, _("Bad clearfree start sector %s.\n"),
				argv[optind]);
		goto fail;
	}
	attrs.start = lnum;

	lnum = cvtnum(file->xfd.fsgeom.blocksize, file->xfd.fsgeom.sectsize,
			argv[optind + 1]);
	if (lnum < 0) {
		fprintf(stderr, _("Bad clearfree length %s.\n"),
				argv[optind + 1]);
		goto fail;
	}
	attrs.length = lnum;

	ret = clearspace_init(&req, &attrs);
	if (ret)
		goto fail;

	for (i = 0; i < nr; i++) {
		ret = clearspace_run(req);
		if (ret)
			goto fail;
	}

	ret = clearspace_efficacy(req, &cleared);
	if (ret)
		goto fail;

	printf(_("Cleared 0x%llx bytes (%.1f%%) from 0x%llx to 0x%llx.\n"),
			cleared, 100.0 * cleared / attrs.length, attrs.start,
			attrs.start + attrs.length);

	ret = clearspace_free(&req);
	if (ret)
		goto fail;

	fshandle_destroy();
	return 0;
fail:
	fshandle_destroy();
	exitcode = 1;
	return 1;
}

static struct cmdinfo clearfree_cmd = {
	.name		= "clearfree",
	.cfunc		= clearfree_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT,
	.args		= "[-n runs] [-r] [-v mask] start length",
	.help		= clearfree_help,
};

void
clearfree_init(void)
{
	clearfree_cmd.oneline = _("clear free space in the filesystem");

	add_command(&clearfree_cmd);
}
