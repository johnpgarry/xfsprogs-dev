// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Red Hat, Inc. All Rights Reserved.
 */

#include <sys/ioctl.h>
#include <sys/mount.h>
#include "platform_defs.h"
#include "libxfs.h"
#include "path.h"
#include "command.h"
#include "init.h"
#include "io.h"

#ifndef FS_IOC_GETFSLABEL
/* Max chars for the interface; fs limits may differ */
#define FSLABEL_MAX 256
#define FS_IOC_GETFSLABEL		_IOR(0x94, 49, char[FSLABEL_MAX])
#define FS_IOC_SETFSLABEL		_IOW(0x94, 50, char[FSLABEL_MAX])
#endif

static cmdinfo_t label_cmd;

static void
label_help(void)
{
	printf(_(
"\n"
" Manipulate or query the filesystem label while mounted.\n"
"\n"
" With no arguments, displays the current filesystem label.\n"
" -s newlabel -- set the filesystem label to newlabel\n"
" -c          -- clear the filesystem label (sets to NULL string)\n"
"\n"));
}

static int
label_f(
	int		argc,
	char		**argv)
{
	int		c;
	int		error;
	char		label[FSLABEL_MAX];

	if (argc == 1) {
		memset(label, 0, sizeof(label));
		error = ioctl(file->fd, FS_IOC_GETFSLABEL, &label);
		goto out;
	}

	while ((c = getopt(argc, argv, "cs:")) != EOF) {
		switch (c) {
		case 'c':
			label[0] = '\0';
			break;
		case 's':
			strncpy(label, optarg, sizeof(label));
			break;
		default:
			return command_usage(&label_cmd);
		}
	}

	/* Check for trailing arguments */
	if (argc != optind)
		return command_usage(&label_cmd);

	error = ioctl(file->fd, FS_IOC_SETFSLABEL, label);
out:
	if (error) {
		perror("label");
		exitcode = 1;
	} else {
		printf("label = \"%s\"\n", label);
	}

	return 0;
}

void
label_init(void)
{
	label_cmd.name = "label";
	label_cmd.cfunc = label_f;
	label_cmd.argmin = 0;
	label_cmd.argmax = 3;
	label_cmd.args = _("[-s label|-c]");
	label_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	label_cmd.oneline =
		_("query, set, or clear the filesystem label while mounted");
	label_cmd.help = label_help;

	add_command(&label_cmd);
}
