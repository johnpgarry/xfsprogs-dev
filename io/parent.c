// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "libfrog/paths.h"
#include "libfrog/getparents.h"
#include "handle.h"
#include "init.h"
#include "io.h"

static cmdinfo_t parent_cmd;
static char *mntpt;

static int
pptr_print(
	const struct parent_rec	*rec,
	void			*arg)
{
	const char		*name = (char *)rec->p_name;
	unsigned int		namelen;

	if (rec->p_flags & PARENT_IS_ROOT) {
		printf(_("Root directory.\n"));
		return 0;
	}

	namelen = strlen(name);
	printf(_("p_ino     = %llu\n"), (unsigned long long)rec->p_ino);
	printf(_("p_gen     = %u\n"), (unsigned int)rec->p_gen);
	printf(_("p_namelen = %u\n"), namelen);
	printf(_("p_name    = \"%s\"\n\n"), rec->p_name);

	return 0;
}

static int
print_parents(
	struct xfs_handle	*handle)
{
	int			ret;

	if (handle)
		ret = handle_walk_parents(handle, sizeof(*handle), pptr_print,
				NULL);
	else
		ret = fd_walk_parents(file->fd, pptr_print, NULL);
	if (ret)
		fprintf(stderr, "%s: %s\n", file->name, strerror(ret));

	return 0;
}

static int
path_print(
	const char		*mntpt,
	const struct path_list	*path,
	void			*arg)
{
	char			buf[PATH_MAX];
	size_t			len = PATH_MAX;
	int			mntpt_len = strlen(mntpt);
	int			ret;

	/* Trim trailing slashes from the mountpoint */
	while (mntpt_len > 0 && mntpt[mntpt_len - 1] == '/')
		mntpt_len--;

	ret = snprintf(buf, len, "%.*s", mntpt_len, mntpt);
	if (ret != mntpt_len)
		return ENAMETOOLONG;

	ret = path_list_to_string(path, buf + ret, len - ret);
	if (ret < 0)
		return ENAMETOOLONG;

	printf("%s\n", buf);
	return 0;
}

static int
print_paths(
	struct xfs_handle	*handle)
{
	int			ret;

	if (handle)
		ret = handle_walk_parent_paths(handle, sizeof(*handle),
				path_print, NULL);
	else
		ret = fd_walk_parent_paths(file->fd, path_print, NULL);
	if (ret)
		fprintf(stderr, "%s: %s\n", file->name, strerror(ret));
	return 0;
}

static int
parent_f(
	int			argc,
	char			**argv)
{
	struct xfs_handle	handle;
	void			*hanp = NULL;
	size_t			hlen;
	struct fs_path		*fs;
	char			*p;
	uint64_t		ino = 0;
	uint32_t		gen = 0;
	int			c;
	int			listpath_flag = 0;
	int			ret;
	static int		tab_init;

	if (!tab_init) {
		tab_init = 1;
		fs_table_initialise(0, NULL, 0, NULL);
	}
	fs = fs_table_lookup(file->name, FS_MOUNT_POINT);
	if (!fs) {
		fprintf(stderr, _("file argument, \"%s\", is not in a mounted XFS filesystem\n"),
			file->name);
		exitcode = 1;
		return 1;
	}
	mntpt = fs->fs_dir;

	while ((c = getopt(argc, argv, "p")) != EOF) {
		switch (c) {
		case 'p':
			listpath_flag = 1;
			break;
		default:
			return command_usage(&parent_cmd);
		}
	}

	/*
	 * Always initialize the fshandle table because we need it for
	 * the ppaths functions to work.
	 */
	ret = path_to_fshandle((char *)mntpt, &hanp, &hlen);
	if (ret) {
		perror(mntpt);
		return 0;
	}

	if (optind + 2 == argc) {
		ino = strtoull(argv[optind], &p, 0);
		if (*p != '\0' || ino == 0) {
			fprintf(stderr,
				_("Bad inode number '%s'.\n"),
				argv[optind]);
			return 0;
		}
		gen = strtoul(argv[optind + 1], &p, 0);
		if (*p != '\0') {
			fprintf(stderr,
				_("Bad generation number '%s'.\n"),
				argv[optind + 1]);
			return 0;
		}

		memcpy(&handle, hanp, sizeof(handle));
		handle.ha_fid.fid_len = sizeof(xfs_fid_t) -
				sizeof(handle.ha_fid.fid_len);
		handle.ha_fid.fid_pad = 0;
		handle.ha_fid.fid_ino = ino;
		handle.ha_fid.fid_gen = gen;

	} else if (optind != argc) {
		return command_usage(&parent_cmd);
	}

	if (listpath_flag)
		exitcode = print_paths(ino ? &handle : NULL);
	else
		exitcode = print_parents(ino ? &handle : NULL);

	if (hanp)
		free_handle(hanp, hlen);

	return 0;
}

static void
parent_help(void)
{
printf(_(
"\n"
" list the current file's parents and their filenames\n"
"\n"
" -p -- list the current file's paths up to the root\n"
"\n"
"If ino and gen are supplied, use them instead.\n"
"\n"));
}

void
parent_init(void)
{
	parent_cmd.name = "parent";
	parent_cmd.cfunc = parent_f;
	parent_cmd.argmin = 0;
	parent_cmd.argmax = -1;
	parent_cmd.args = _("[-p] [ino gen]");
	parent_cmd.flags = CMD_NOMAP_OK;
	parent_cmd.oneline = _("print parent inodes");
	parent_cmd.help = parent_help;

	add_command(&parent_cmd);
}
