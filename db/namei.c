// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "command.h"
#include "output.h"
#include "init.h"
#include "io.h"
#include "type.h"
#include "input.h"
#include "faddr.h"
#include "fprint.h"
#include "field.h"
#include "inode.h"

/* Path lookup */

/* Key for looking up metadata inodes. */
struct dirpath {
	/* Array of string pointers. */
	char		**path;

	/* Number of strings in path. */
	unsigned int	depth;
};

static void
path_free(
	struct dirpath	*dirpath)
{
	unsigned int	i;

	for (i = 0; i < dirpath->depth; i++)
		free(dirpath->path[i]);
	free(dirpath->path);
	free(dirpath);
}

/* Chop a freeform string path into a structured path. */
static struct dirpath *
path_parse(
	const char	*path)
{
	struct dirpath	*dirpath;
	const char	*p = path;
	const char	*endp = path + strlen(path);

	dirpath = calloc(sizeof(*dirpath), 1);
	if (!dirpath)
		return NULL;

	while (p < endp) {
		char		**new_path;
		const char	*next_slash;

		next_slash = strchr(p, '/');
		if (next_slash == p) {
			p++;
			continue;
		}
		if (!next_slash)
			next_slash = endp;

		new_path = realloc(dirpath->path,
				(dirpath->depth + 1) * sizeof(char *));
		if (!new_path) {
			path_free(dirpath);
			return NULL;
		}

		dirpath->path = new_path;
		dirpath->path[dirpath->depth] = strndup(p, next_slash - p);
		dirpath->depth++;

		p = next_slash + 1;
	}

	return dirpath;
}

/* Given a directory and a structured path, walk the path and set the cursor. */
static int
path_navigate(
	struct xfs_mount	*mp,
	xfs_ino_t		rootino,
	struct dirpath		*dirpath)
{
	struct xfs_inode	*dp;
	xfs_ino_t		ino = rootino;
	unsigned int		i;
	int			error;

	error = -libxfs_iget(mp, NULL, ino, 0, &dp);
	if (error)
		return error;

	for (i = 0; i < dirpath->depth; i++) {
		struct xfs_name	xname = {
			.name	= dirpath->path[i],
			.len	= strlen(dirpath->path[i]),
		};

		if (!S_ISDIR(VFS_I(dp)->i_mode)) {
			error = ENOTDIR;
			goto rele;
		}

		error = -libxfs_dir_lookup(NULL, dp, &xname, &ino, NULL);
		if (error)
			goto rele;
		if (!xfs_verify_ino(mp, ino)) {
			error = EFSCORRUPTED;
			goto rele;
		}

		libxfs_irele(dp);
		dp = NULL;

		error = -libxfs_iget(mp, NULL, ino, 0, &dp);
		switch (error) {
		case EFSCORRUPTED:
		case EFSBADCRC:
		case 0:
			break;
		default:
			return error;
		}
	}

	set_cur_inode(ino);
rele:
	if (dp)
		libxfs_irele(dp);
	return error;
}

/* Walk a directory path to an inode and set the io cursor to that inode. */
static int
path_walk(
	char		*path)
{
	struct dirpath	*dirpath;
	char		*p = path;
	xfs_ino_t	rootino = mp->m_sb.sb_rootino;
	int		error = 0;

	if (*p == '/') {
		/* Absolute path, start from the root inode. */
		p++;
	} else {
		/* Relative path, start from current dir. */
		if (iocur_top->typ != &typtab[TYP_INODE] ||
		    !S_ISDIR(iocur_top->mode))
			return ENOTDIR;

		rootino = iocur_top->ino;
	}

	dirpath = path_parse(p);
	if (!dirpath)
		return ENOMEM;

	error = path_navigate(mp, rootino, dirpath);
	if (error)
		return error;

	path_free(dirpath);
	return 0;
}

static void
path_help(void)
{
	dbprintf(_(
"\n"
" Navigate to an inode via directory path.\n"
	));
}

static int
path_f(
	int		argc,
	char		**argv)
{
	int		c;
	int		error;

	while ((c = getopt(argc, argv, "")) != -1) {
		switch (c) {
		default:
			path_help();
			return 0;
		}
	}

	error = path_walk(argv[optind]);
	if (error) {
		dbprintf("%s: %s\n", argv[optind], strerror(error));
		exitcode = 1;
	}

	return 0;
}

static struct cmdinfo path_cmd = {
	.name		= "path",
	.altname	= NULL,
	.cfunc		= path_f,
	.argmin		= 1,
	.argmax		= 1,
	.canpush	= 0,
	.args		= "",
	.help		= path_help,
};

void
namei_init(void)
{
	path_cmd.oneline = _("navigate to an inode by path");
	add_command(&path_cmd);
}
