// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2004-2005 Silicon Graphics, Inc.
 * Copyright (c) 2012 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "xfrog.h"
#include <sys/mman.h>
#include "command.h"
#include "input.h"
#include "init.h"
#include "path.h"
#include "space.h"

static cmdinfo_t print_cmd;

struct fileio	*filetable;
int		filecount;
struct fileio	*file;

static void
print_fileio(
	struct fileio	*file,
	int		index,
	int		braces)
{
	printf(_("%c%03d%c %-14s\n"), braces? '[' : ' ', index,
			braces? ']' : ' ', file->name);
}

static int
print_f(
	int		argc,
	char		**argv)
{
	int		i;

	for (i = 0; i < filecount; i++)
		print_fileio(&filetable[i], i, &filetable[i] == file);
	return 0;
}

int
openfile(
	char		*path,
	struct xfs_fsop_geom *geom,
	struct fs_path	*fs_path)
{
	struct fs_path	*fsp;
	int		fd;

	fd = open(path, 0);
	if (fd < 0) {
		perror(path);
		return -1;
	}

	if (xfrog_geometry(fd, geom) < 0) {
		if (errno == ENOTTY)
			fprintf(stderr,
_("%s: Not on a mounted XFS filesystem.\n"),
					path);
		else
			perror("XFS_IOC_FSGEOMETRY");
		close(fd);
		return -1;
	}

	if (fs_path) {
		fsp = fs_table_lookup(path, FS_MOUNT_POINT);
		if (!fsp) {
			fprintf(stderr, _("%s: cannot find mount point."),
				path);
			close(fd);
			return -1;
		}
		memcpy(fs_path, fsp, sizeof(struct fs_path));
	}
	return fd;
}

int
addfile(
	char		*name,
	int		fd,
	struct xfs_fsop_geom *geometry,
	struct fs_path	*fs_path)
{
	char		*filename;

	filename = strdup(name);
	if (!filename) {
		perror("strdup");
		close(fd);
		return -1;
	}

	/* Extend the table of currently open files */
	filetable = (struct fileio *)realloc(filetable,	/* growing */
					++filecount * sizeof(struct fileio));
	if (!filetable) {
		perror("realloc");
		filecount = 0;
		free(filename);
		close(fd);
		return -1;
	}

	/* Finally, make this the new active open file */
	file = &filetable[filecount - 1];
	file->fd = fd;
	file->name = filename;
	file->geom = *geometry;
	memcpy(&file->fs_path, fs_path, sizeof(file->fs_path));
	return 0;
}

void
print_init(void)
{
	print_cmd.name = "print";
	print_cmd.altname = "p";
	print_cmd.cfunc = print_f;
	print_cmd.argmin = 0;
	print_cmd.argmax = 0;
	print_cmd.flags = CMD_FLAG_ONESHOT;
	print_cmd.oneline = _("list current open files");

	add_command(&print_cmd);
}
