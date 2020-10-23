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
	int		ret = 0;

	if (*p == '/') {
		/* Absolute path, start from the root inode. */
		p++;
	} else {
		/* Relative path, start from current dir. */
		if (iocur_top->typ != &typtab[TYP_INODE]) {
			dbprintf(_("current object is not an inode.\n"));
			return -1;
		}

		if (!S_ISDIR(iocur_top->mode)) {
			dbprintf(_("current inode %llu is not a directory.\n"),
					(unsigned long long)iocur_top->ino);
			return -1;
		}
		rootino = iocur_top->ino;
	}

	dirpath = path_parse(p);
	if (!dirpath) {
		dbprintf(_("%s: not enough memory to parse.\n"), path);
		return -1;
	}

	ret = path_navigate(mp, rootino, dirpath);
	if (ret) {
		dbprintf(_("%s: %s\n"), path, strerror(ret));
		ret = -1;
	}

	path_free(dirpath);
	return ret;
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

	while ((c = getopt(argc, argv, "")) != -1) {
		switch (c) {
		default:
			path_help();
			return 0;
		}
	}

	if (path_walk(argv[optind]))
		exitcode = 1;
	return 0;
}

static const cmdinfo_t path_cmd = {
	.name		= "path",
	.altname	= NULL,
	.cfunc		= path_f,
	.argmin		= 1,
	.argmax		= 1,
	.canpush	= 0,
	.args		= "",
	.oneline	= N_("navigate to an inode by path"),
	.help		= path_help,
};

/* List a directory's entries. */

static const char *filetype_strings[XFS_DIR3_FT_MAX] = {
	[XFS_DIR3_FT_UNKNOWN]	= N_("unknown"),
	[XFS_DIR3_FT_REG_FILE]	= N_("regular"),
	[XFS_DIR3_FT_DIR]	= N_("directory"),
	[XFS_DIR3_FT_CHRDEV]	= N_("chardev"),
	[XFS_DIR3_FT_BLKDEV]	= N_("blkdev"),
	[XFS_DIR3_FT_FIFO]	= N_("fifo"),
	[XFS_DIR3_FT_SOCK]	= N_("socket"),
	[XFS_DIR3_FT_SYMLINK]	= N_("symlink"),
	[XFS_DIR3_FT_WHT]	= N_("whiteout"),
};

static const char *
get_dstr(
	struct xfs_mount	*mp,
	uint8_t			filetype)
{
	if (!xfs_sb_version_hasftype(&mp->m_sb))
		return filetype_strings[XFS_DIR3_FT_UNKNOWN];

	if (filetype >= XFS_DIR3_FT_MAX)
		return filetype_strings[XFS_DIR3_FT_UNKNOWN];

	return filetype_strings[filetype];
}

static void
dir_emit(
	struct xfs_mount	*mp,
	char			*name,
	ssize_t			namelen,
	xfs_ino_t		ino,
	uint8_t			dtype)
{
	char			*display_name;
	struct xfs_name		xname = { .name = name };
	const char		*dstr = get_dstr(mp, dtype);
	xfs_dahash_t		hash;
	bool			good;

	if (namelen < 0) {
		/* Negative length means that name is null-terminated. */
		display_name = name;
		xname.len = strlen(name);
		good = true;
	} else {
		/*
		 * Otherwise, name came from a directory entry, so we have to
		 * copy the string to a buffer so that we can add the null
		 * terminator.
		 */
		display_name = malloc(namelen + 1);
		memcpy(display_name, name, namelen);
		display_name[namelen] = 0;
		xname.len = namelen;
		good = libxfs_dir2_namecheck(name, namelen);
	}
	hash = libxfs_dir2_hashname(mp, &xname);

	dbprintf("%-18llu %-14s 0x%08llx %3d %s", ino, dstr, hash, xname.len,
			display_name);
	if (!good)
		dbprintf(_(" (corrupt)"));
	dbprintf("\n");

	if (display_name != name)
		free(display_name);
}

static int
list_sfdir(
	struct xfs_da_args		*args)
{
	struct xfs_inode		*dp = args->dp;
	struct xfs_mount		*mp = dp->i_mount;
	struct xfs_dir2_sf_entry	*sfep;
	struct xfs_dir2_sf_hdr		*sfp;
	xfs_ino_t			ino;
	unsigned int			i;
	uint8_t				filetype;

	sfp = (struct xfs_dir2_sf_hdr *)dp->i_df.if_u1.if_data;

	/* . and .. entries */
	dir_emit(args->dp->i_mount, ".", -1, dp->i_ino, XFS_DIR3_FT_DIR);

	ino = libxfs_dir2_sf_get_parent_ino(sfp);
	dir_emit(args->dp->i_mount, "..", -1, ino, XFS_DIR3_FT_DIR);

	/* Walk everything else. */
	sfep = xfs_dir2_sf_firstentry(sfp);
	for (i = 0; i < sfp->count; i++) {
		ino = libxfs_dir2_sf_get_ino(mp, sfp, sfep);
		filetype = libxfs_dir2_sf_get_ftype(mp, sfep);

		dir_emit(args->dp->i_mount, (char *)sfep->name, sfep->namelen,
				ino, filetype);
		sfep = libxfs_dir2_sf_nextentry(mp, sfp, sfep);
	}

	return 0;
}

/* List entries in block format directory. */
static int
list_blockdir(
	struct xfs_da_args	*args)
{
	struct xfs_inode	*dp = args->dp;
	struct xfs_mount	*mp = dp->i_mount;
	struct xfs_buf		*bp;
	struct xfs_da_geometry	*geo = mp->m_dir_geo;
	unsigned int		offset;
	unsigned int		end;
	int			error;

	error = xfs_dir3_block_read(NULL, dp, &bp);
	if (error)
		return error;

	end = xfs_dir3_data_end_offset(geo, bp->b_addr);
	for (offset = geo->data_entry_offset; offset < end;) {
		struct xfs_dir2_data_unused	*dup = bp->b_addr + offset;
		struct xfs_dir2_data_entry	*dep = bp->b_addr + offset;
		uint8_t				filetype;

		if (be16_to_cpu(dup->freetag) == XFS_DIR2_DATA_FREE_TAG) {
			/* Unused entry */
			offset += be16_to_cpu(dup->length);
			continue;
		}

		/* Real entry */
		offset += libxfs_dir2_data_entsize(mp, dep->namelen);
		filetype = libxfs_dir2_data_get_ftype(dp->i_mount, dep);
		dir_emit(mp, (char *)dep->name, dep->namelen,
				be64_to_cpu(dep->inumber), filetype);
	}

	libxfs_trans_brelse(args->trans, bp);
	return error;
}

/* List entries in leaf format directory. */
static int
list_leafdir(
	struct xfs_da_args	*args)
{
	struct xfs_bmbt_irec	map;
	struct xfs_iext_cursor	icur;
	struct xfs_inode	*dp = args->dp;
	struct xfs_mount	*mp = dp->i_mount;
	struct xfs_buf		*bp = NULL;
	struct xfs_ifork	*ifp = XFS_IFORK_PTR(dp, XFS_DATA_FORK);
	struct xfs_da_geometry	*geo = mp->m_dir_geo;
	xfs_dablk_t		dabno = 0;
	int			error = 0;

	/* Read extent map. */
	if (!(ifp->if_flags & XFS_IFEXTENTS)) {
		error = -libxfs_iread_extents(NULL, dp, XFS_DATA_FORK);
		if (error)
			return error;
	}

	while (dabno < geo->leafblk) {
		unsigned int	offset;
		unsigned int	length;

		/* Find mapping for leaf block. */
		if (!xfs_iext_lookup_extent(dp, ifp, dabno, &icur, &map))
			break;
		if (map.br_startoff >= geo->leafblk)
			break;
		libxfs_trim_extent(&map, dabno, geo->leafblk - dabno);

		/* Read the directory block of that first mapping. */
		error = xfs_dir3_data_read(NULL, dp, map.br_startoff, 0, &bp);
		if (error)
			break;

		for (offset = geo->data_entry_offset; offset < geo->blksize;) {
			struct xfs_dir2_data_entry	*dep;
			struct xfs_dir2_data_unused	*dup;
			uint8_t				filetype;

			dup = bp->b_addr + offset;
			dep = bp->b_addr + offset;

			if (be16_to_cpu(dup->freetag) ==
			    XFS_DIR2_DATA_FREE_TAG) {
				/* Skip unused entry */
				length = be16_to_cpu(dup->length);
				offset += length;
				continue;
			}

			offset += libxfs_dir2_data_entsize(mp, dep->namelen);
			filetype = libxfs_dir2_data_get_ftype(mp, dep);

			dir_emit(mp, (char *)dep->name, dep->namelen,
					be64_to_cpu(dep->inumber), filetype);
		}

		dabno += XFS_DADDR_TO_FSB(mp, bp->b_length);
		libxfs_buf_relse(bp);
		bp = NULL;
	}

	if (bp)
		libxfs_buf_relse(bp);

	return error;
}

/* Read the directory, display contents. */
int
listdir(
	struct xfs_inode	*dp)
{
	struct xfs_da_args	args = {
		.dp		= dp,
		.geo		= dp->i_mount->m_dir_geo,
	};
	int			error;
	int			isblock;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL)
		return list_sfdir(&args);

	error = -libxfs_dir2_isblock(&args, &isblock);
	if (error)
		return error;

	if (isblock)
		return list_blockdir(&args);
	return list_leafdir(&args);
}

/* If the io cursor points to a directory, list its contents. */
static int
ls_cur(
	char			*tag,
	bool			direct)
{
	struct xfs_inode	*dp;
	int			ret = 0;

	if (iocur_top->typ != &typtab[TYP_INODE]) {
		dbprintf(_("current object is not an inode.\n"));
		return -1;
	}

	ret = -libxfs_iget(mp, NULL, iocur_top->ino, 0, &dp);
	if (ret) {
		dbprintf(_("failed to iget directory %llu, error %d\n"),
				(unsigned long long)iocur_top->ino, ret);
		return -1;
	}

	if (S_ISDIR(VFS_I(dp)->i_mode) && !direct) {
		/* List the contents of a directory. */
		if (tag)
			dbprintf(_("%s:\n"), tag);

		ret = listdir(dp);
		if (ret) {
			dbprintf(_("failed to list directory %llu: %s\n"),
					(unsigned long long)iocur_top->ino,
					strerror(ret));
			ret = -1;
			goto rele;
		}
	} else if (direct || !S_ISDIR(VFS_I(dp)->i_mode)) {
		/* List the directory entry associated with a single file. */
		char		inum[32];

		if (!tag) {
			snprintf(inum, sizeof(inum), "<%llu>",
					(unsigned long long)iocur_top->ino);
			tag = inum;
		} else {
			char	*p = strrchr(tag, '/');

			if (p)
				tag = p + 1;
		}

		dir_emit(mp, tag, -1, iocur_top->ino,
				libxfs_mode_to_ftype(VFS_I(dp)->i_mode));
	} else {
		dbprintf(_("current inode %llu is not a directory.\n"),
				(unsigned long long)iocur_top->ino);
		ret = -1;
		goto rele;
	}

rele:
	libxfs_irele(dp);
	return ret;
}

static void
ls_help(void)
{
	dbprintf(_(
"\n"
" List the contents of the currently selected directory inode.\n"
"\n"
" Options:\n"
"   -d -- List directories themselves, not their contents.\n"
"\n"
" Directory contents will be listed in the format:\n"
" inode_number	type	hash	name_length	name\n"
	));
}

static int
ls_f(
	int			argc,
	char			**argv)
{
	bool			direct = false;
	int			c;
	int			ret = 0;

	while ((c = getopt(argc, argv, "d")) != -1) {
		switch (c) {
		case 'd':
			direct = true;
			break;
		default:
			ls_help();
			return 0;
		}
	}

	if (optind == argc) {
		if (ls_cur(NULL, direct))
			exitcode = 1;
		return 0;
	}

	for (c = optind; c < argc; c++) {
		push_cur();

		ret = path_walk(argv[c]);
		if (ret)
			goto err_cur;

		ret = ls_cur(argv[c], direct);
		if (ret)
			goto err_cur;

		pop_cur();
	}

	return 0;
err_cur:
	pop_cur();
	if (ret)
		exitcode = 1;
	return 0;
}

static const cmdinfo_t ls_cmd = {
	.name		= "ls",
	.altname	= "l",
	.cfunc		= ls_f,
	.argmin		= 0,
	.argmax		= -1,
	.canpush	= 0,
	.args		= "[-d] [paths...]",
	.oneline	= N_("list directory contents"),
	.help		= ls_help,
};

void
namei_init(void)
{
	add_command(&path_cmd);
	add_command(&ls_cmd);
}
