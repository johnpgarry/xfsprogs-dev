// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2012 Red Hat, Inc
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "input.h"
#include "init.h"
#include "libfrog/paths.h"
#include "space.h"

char	*progname;
int	exitcode;

static void
usage(void)
{
	fprintf(stderr,
		_("Usage: %s [-c cmd] file\n"),
		progname);
	exit(1);
}

static void
init_commands(void)
{
	print_init();
	help_init();
	info_init();
	prealloc_init();
	quit_init();
	trim_init();
	freesp_init();
}

static int
init_args_command(
	int	index)
{
	if (index >= filecount)
		return 0;
	file = &filetable[index++];
	return index;
}

static int
init_check_command(
	const cmdinfo_t	*ct)
{
	if (!(ct->flags & CMD_FLAG_ONESHOT))
		return 0;
	return 1;
}

static void
init(
	int		argc,
	char		**argv)
{
	int		c;
	struct xfs_fsop_geom geometry = { 0 };
	struct fs_path	fsp;

	progname = basename(argv[0]);
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	fs_table_initialise(0, NULL, 0, NULL);
	while ((c = getopt(argc, argv, "c:p:V")) != EOF) {
		switch (c) {
		case 'c':
			add_user_command(optarg);
			break;
		case 'p':
			progname = optarg;
			break;
		case 'V':
			printf(_("%s version %s\n"), progname, VERSION);
			exit(0);
		default:
			usage();
		}
	}

	if (optind != argc - 1)
		usage();

	if ((c = openfile(argv[optind], &geometry, &fsp)) < 0)
		exit(1);
	if (!platform_test_xfs_fd(c))
		printf(_("Not an XFS filesystem!\n"));
	if (addfile(argv[optind], c, &geometry, &fsp) < 0)
		exit(1);

	init_commands();
	add_command_iterator(init_args_command);
	add_check_command(init_check_command);
}

int
main(
	int	argc,
	char	**argv)
{
	init(argc, argv);
	command_loop();
	return exitcode;
}
