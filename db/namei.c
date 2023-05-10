// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
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
			.name	= (unsigned char *)dirpath->path[i],
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
	path_free(dirpath);
	return error;
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

/* List a directory's entries. */

static const char *filetype_strings[XFS_DIR3_FT_MAX] = {
	[XFS_DIR3_FT_UNKNOWN]	= "unknown",
	[XFS_DIR3_FT_REG_FILE]	= "regular",
	[XFS_DIR3_FT_DIR]	= "directory",
	[XFS_DIR3_FT_CHRDEV]	= "chardev",
	[XFS_DIR3_FT_BLKDEV]	= "blkdev",
	[XFS_DIR3_FT_FIFO]	= "fifo",
	[XFS_DIR3_FT_SOCK]	= "socket",
	[XFS_DIR3_FT_SYMLINK]	= "symlink",
	[XFS_DIR3_FT_WHT]	= "whiteout",
};

static const char *
get_dstr(
	struct xfs_mount	*mp,
	uint8_t			filetype)
{
	if (!xfs_has_ftype(mp))
		return filetype_strings[XFS_DIR3_FT_UNKNOWN];

	if (filetype >= XFS_DIR3_FT_MAX)
		return filetype_strings[XFS_DIR3_FT_UNKNOWN];

	return filetype_strings[filetype];
}

static void
dir_emit(
	struct xfs_mount	*mp,
	xfs_dir2_dataptr_t	off,
	char			*name,
	ssize_t			namelen,
	xfs_ino_t		ino,
	uint8_t			dtype)
{
	char			*display_name;
	struct xfs_name		xname = { .name = (unsigned char *)name };
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

	dbprintf("%-10u %-18llu %-14s 0x%08llx %3d %s %s\n", off & 0xFFFFFFFF,
			ino, dstr, hash, xname.len,
			display_name, good ? _("(good)") : _("(corrupt)"));

	if (display_name != name)
		free(display_name);
}

static int
list_sfdir(
	struct xfs_da_args		*args)
{
	struct xfs_inode		*dp = args->dp;
	struct xfs_mount		*mp = dp->i_mount;
	struct xfs_da_geometry		*geo = args->geo;
	struct xfs_dir2_sf_entry	*sfep;
	struct xfs_dir2_sf_hdr		*sfp;
	xfs_ino_t			ino;
	xfs_dir2_dataptr_t		off;
	unsigned int			i;
	uint8_t				filetype;

	sfp = (struct xfs_dir2_sf_hdr *)dp->i_df.if_u1.if_data;

	/* . and .. entries */
	off = xfs_dir2_db_off_to_dataptr(geo, geo->datablk,
			geo->data_entry_offset);
	dir_emit(args->dp->i_mount, off, ".", -1, dp->i_ino, XFS_DIR3_FT_DIR);

	ino = libxfs_dir2_sf_get_parent_ino(sfp);
	off = xfs_dir2_db_off_to_dataptr(geo, geo->datablk,
			geo->data_entry_offset +
			libxfs_dir2_data_entsize(mp, sizeof(".") - 1));
	dir_emit(args->dp->i_mount, off, "..", -1, ino, XFS_DIR3_FT_DIR);

	/* Walk everything else. */
	sfep = xfs_dir2_sf_firstentry(sfp);
	for (i = 0; i < sfp->count; i++) {
		ino = libxfs_dir2_sf_get_ino(mp, sfp, sfep);
		filetype = libxfs_dir2_sf_get_ftype(mp, sfep);
		off = xfs_dir2_db_off_to_dataptr(geo, geo->datablk,
				xfs_dir2_sf_get_offset(sfep));

		dir_emit(args->dp->i_mount, off, (char *)sfep->name,
				sfep->namelen, ino, filetype);
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
	xfs_dir2_dataptr_t	diroff;
	unsigned int		offset;
	unsigned int		end;
	int			error;

	error = xfs_dir3_block_read(NULL, dp, args->owner, &bp);
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
		diroff = xfs_dir2_db_off_to_dataptr(geo, geo->datablk, offset);
		offset += libxfs_dir2_data_entsize(mp, dep->namelen);
		filetype = libxfs_dir2_data_get_ftype(dp->i_mount, dep);
		dir_emit(mp, diroff, (char *)dep->name, dep->namelen,
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
	struct xfs_ifork	*ifp = xfs_ifork_ptr(dp, XFS_DATA_FORK);
	struct xfs_da_geometry	*geo = mp->m_dir_geo;
	xfs_dir2_off_t		dirboff;
	xfs_dablk_t		dabno = 0;
	int			error = 0;

	/* Read extent map. */
	error = -libxfs_iread_extents(NULL, dp, XFS_DATA_FORK);
	if (error)
		return error;

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
		error = xfs_dir3_data_read(NULL, dp, args->owner,
				map.br_startoff, 0, &bp);
		if (error)
			break;

		dirboff = xfs_dir2_da_to_byte(geo, map.br_startoff);
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

			dir_emit(mp, xfs_dir2_byte_to_dataptr(dirboff + offset),
					(char *)dep->name, dep->namelen,
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
static int
listdir(
	struct xfs_inode	*dp)
{
	struct xfs_da_args	args = {
		.dp		= dp,
		.geo		= dp->i_mount->m_dir_geo,
		.owner		= dp->i_ino,
	};
	int			error;
	bool			isblock;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL)
		return list_sfdir(&args);

	error = -libxfs_dir2_isblock(&args, &isblock);
	if (error)
		return error;

	if (isblock)
		return list_blockdir(&args);
	return list_leafdir(&args);
}

/* List the inode number of the currently selected inode. */
static int
inum_cur(void)
{
	if (iocur_top->typ != &typtab[TYP_INODE])
		return ENOENT;

	dbprintf("%llu\n", iocur_top->ino);
	return 0;
}

/* If the io cursor points to a directory, list its contents. */
static int
ls_cur(
	char			*tag)
{
	struct xfs_inode	*dp;
	int			error = 0;

	if (iocur_top->typ != &typtab[TYP_INODE] ||
	    !S_ISDIR(iocur_top->mode))
		return ENOTDIR;

	error = -libxfs_iget(mp, NULL, iocur_top->ino, 0, &dp);
	if (error)
		return error;

	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		error = ENOTDIR;
		goto rele;
	}

	/* List the contents of a directory. */
	if (tag)
		dbprintf(_("%s:\n"), tag);

	error = listdir(dp);
	if (error)
		goto rele;

rele:
	libxfs_irele(dp);
	return error;
}

static void
ls_help(void)
{
	dbprintf(_(
"\n"
" List the contents of the currently selected directory inode.\n"
"\n"
" Options:\n"
"   -i -- Resolve the given paths to their corresponding inode numbers.\n"
"         If no paths are given, display the current inode number.\n"
"\n"
" Directory contents will be listed in the format:\n"
" dir_cookie	inode_number	type	hash	name_length	name\n"
	));
}

static int
ls_f(
	int			argc,
	char			**argv)
{
	bool			inum_only = false;
	int			c;
	int			error = 0;

	while ((c = getopt(argc, argv, "i")) != -1) {
		switch (c) {
		case 'i':
			inum_only = true;
			break;
		default:
			ls_help();
			return 0;
		}
	}

	if (optind == argc) {
		if (inum_only)
			error = inum_cur();
		else
			error = ls_cur(NULL);
		if (error) {
			dbprintf("%s\n", strerror(error));
			exitcode = 1;
		}

		return 0;
	}

	for (c = optind; c < argc; c++) {
		push_cur();

		error = path_walk(argv[c]);
		if (error)
			goto err_cur;

		if (inum_only)
			error = inum_cur();
		else
			error = ls_cur(argv[c]);
		if (error)
			goto err_cur;

		pop_cur();
	}

	return 0;
err_cur:
	pop_cur();
	if (error) {
		dbprintf("%s: %s\n", argv[c], strerror(error));
		exitcode = 1;
	}
	return 0;
}

static struct cmdinfo ls_cmd = {
	.name		= "ls",
	.altname	= "l",
	.cfunc		= ls_f,
	.argmin		= 0,
	.argmax		= -1,
	.canpush	= 0,
	.args		= "[-i] [paths...]",
	.help		= ls_help,
};

static void
pptr_emit(
	struct xfs_mount	*mp,
	const struct xfs_parent_name_irec *irec)
{
	struct xfs_name		xname = {
		.name		= irec->p_name,
		.len		= irec->p_namelen,
	};
	xfs_dahash_t		hash;
	bool			good;

	hash = libxfs_dir2_hashname(mp, &xname);
	good = libxfs_parent_verify_irec(mp, irec);

	dbprintf("%18llu:0x%08x 0x%08x:0x%08x %3d %.*s %s\n",
			irec->p_ino, irec->p_gen, irec->p_namehash, hash,
			xname.len, xname.len, xname.name,
			good ? _("(good)") : _("(corrupt)"));
}

static int
list_sf_pptrs(
	struct xfs_inode		*ip)
{
	struct xfs_parent_name_irec	irec;
	struct xfs_attr_shortform	*sf;
	struct xfs_attr_sf_entry	*sfe;
	unsigned int			i;

	sf = (struct xfs_attr_shortform *)ip->i_af.if_u1.if_data;
	for (i = 0, sfe = &sf->list[0]; i < sf->hdr.count; i++) {
		void			*name = sfe->nameval;
		void			*value = &sfe->nameval[sfe->namelen];

		if ((sfe->flags & XFS_ATTR_PARENT) &&
		    libxfs_parent_namecheck(mp, name, sfe->namelen, sfe->flags) &&
		    libxfs_parent_valuecheck(mp, value, sfe->valuelen)) {
			libxfs_parent_irec_from_disk(&irec, name, value,
					sfe->valuelen);
			pptr_emit(mp, &irec);
		}

		sfe = xfs_attr_sf_nextentry(sfe);
	}

	return 0;
}

static void
list_leaf_pptr_entries(
	struct xfs_inode		*ip,
	struct xfs_buf			*bp)
{
	struct xfs_parent_name_irec	irec;
	struct xfs_attr3_icleaf_hdr	ichdr;
	struct xfs_mount		*mp = ip->i_mount;
	struct xfs_attr_leafblock	*leaf = bp->b_addr;
	struct xfs_attr_leaf_entry	*entry;
	unsigned int			i;

	libxfs_attr3_leaf_hdr_from_disk(mp->m_attr_geo, &ichdr, leaf);
	entry = xfs_attr3_leaf_entryp(leaf);

	for (i = 0; i < ichdr.count; entry++, i++) {
		struct xfs_attr_leaf_name_local	*name_loc;
		void			*value;
		void			*name;
		unsigned int		namelen, valuelen;

		if (!(entry->flags & XFS_ATTR_LOCAL) ||
		    !(entry->flags & XFS_ATTR_PARENT))
			continue;

		name_loc = xfs_attr3_leaf_name_local(leaf, i);
		name = name_loc->nameval;
		namelen = name_loc->namelen;
		value = &name_loc->nameval[name_loc->namelen];
		valuelen = be16_to_cpu(name_loc->valuelen);

		if (libxfs_parent_namecheck(mp, name, namelen, entry->flags) &&
		    libxfs_parent_valuecheck(mp, value, valuelen)) {
			libxfs_parent_irec_from_disk(&irec, name, value,
					valuelen);
			pptr_emit(mp, &irec);
		}
	}
}

static int
list_leaf_pptrs(
	struct xfs_inode		*ip)
{
	struct xfs_buf			*leaf_bp;
	int				error;

	error = -libxfs_attr3_leaf_read(NULL, ip, ip->i_ino, 0, &leaf_bp);
	if (error)
		return error;

	list_leaf_pptr_entries(ip, leaf_bp);
	libxfs_trans_brelse(NULL, leaf_bp);
	return 0;
}

static int
find_leftmost_attr_leaf(
	struct xfs_inode		*ip,
	struct xfs_buf			**leaf_bpp)
{
	struct xfs_da3_icnode_hdr	nodehdr;
	struct xfs_mount		*mp = ip->i_mount;
	struct xfs_da_intnode		*node;
	struct xfs_da_node_entry	*btree;
	struct xfs_buf			*bp;
	xfs_dablk_t			blkno = 0;
	unsigned int			expected_level = 0;
	int				error;

	for (;;) {
		uint16_t		magic;

		error = -libxfs_da3_node_read(NULL, ip, blkno, &bp,
				XFS_ATTR_FORK);
		if (error)
			return error;

		node = bp->b_addr;
		magic = be16_to_cpu(node->hdr.info.magic);
		if (magic == XFS_ATTR_LEAF_MAGIC ||
		    magic == XFS_ATTR3_LEAF_MAGIC)
			break;

		error = EFSCORRUPTED;
		if (magic != XFS_DA_NODE_MAGIC &&
		    magic != XFS_DA3_NODE_MAGIC)
			goto out_buf;

		libxfs_da3_node_hdr_from_disk(mp, &nodehdr, node);

		if (nodehdr.count == 0 || nodehdr.level >= XFS_DA_NODE_MAXDEPTH)
			goto out_buf;

		/* Check the level from the root node. */
		if (blkno == 0)
			expected_level = nodehdr.level - 1;
		else if (expected_level != nodehdr.level)
			goto out_buf;
		else
			expected_level--;

		/* Find the next level towards the leaves of the dabtree. */
		btree = nodehdr.btree;
		blkno = be32_to_cpu(btree->before);
		libxfs_trans_brelse(NULL, bp);
	}

	error = EFSCORRUPTED;
	if (expected_level != 0)
		goto out_buf;

	*leaf_bpp = bp;
	return 0;

out_buf:
	libxfs_trans_brelse(NULL, bp);
	return error;
}

static int
list_node_pptrs(
	struct xfs_inode		*ip)
{
	struct xfs_attr3_icleaf_hdr	leafhdr;
	struct xfs_mount		*mp = ip->i_mount;
	struct xfs_attr_leafblock	*leaf;
	struct xfs_buf			*leaf_bp;
	int				error;

	error = find_leftmost_attr_leaf(ip, &leaf_bp);
	if (error)
		return error;

	for (;;) {
		list_leaf_pptr_entries(ip, leaf_bp);

		/* Find the right sibling of this leaf block. */
		leaf = leaf_bp->b_addr;
		libxfs_attr3_leaf_hdr_from_disk(mp->m_attr_geo, &leafhdr, leaf);
		if (leafhdr.forw == 0)
			goto out_leaf;

		libxfs_trans_brelse(NULL, leaf_bp);

		error = -libxfs_attr3_leaf_read(NULL, ip, ip->i_ino,
				leafhdr.forw, &leaf_bp);
		if (error)
			return error;
	}

out_leaf:
	libxfs_trans_brelse(NULL, leaf_bp);
	return error;
}

static int
list_pptrs(
	struct xfs_inode	*ip)
{
	int			error;

	if (!libxfs_inode_hasattr(ip))
		return 0;

	if (ip->i_af.if_format == XFS_DINODE_FMT_LOCAL)
		return list_sf_pptrs(ip);

	/* attr functions require that the attr fork is loaded */
	error = -libxfs_iread_extents(NULL, ip, XFS_ATTR_FORK);
	if (error)
		return error;

	if (libxfs_attr_is_leaf(ip))
		return list_leaf_pptrs(ip);

	return list_node_pptrs(ip);
}

/* If the io cursor points to a file, list its parents. */
static int
parent_cur(
	char			*tag)
{
	struct xfs_inode	*ip;
	int			error = 0;

	if (!xfs_has_parent(mp))
		return 0;

	if (iocur_top->typ != &typtab[TYP_INODE])
		return ENOTDIR;

	error = -libxfs_iget(mp, NULL, iocur_top->ino, 0, &ip);
	if (error)
		return error;

	/* List the parents of a file. */
	if (tag)
		dbprintf(_("%s:\n"), tag);

	error = list_pptrs(ip);
	if (error)
		goto rele;

rele:
	libxfs_irele(ip);
	return error;
}

static void
parent_help(void)
{
	dbprintf(_(
"\n"
" List the parents of the currently selected file.\n"
"\n"
" Parent pointers will be listed in the format:\n"
" inode_number:inode_gen	ondisk_namehash:namehash	name_length	name\n"
	));
}

static int
parent_f(
	int			argc,
	char			**argv)
{
	int			c;
	int			error = 0;

	while ((c = getopt(argc, argv, "")) != -1) {
		switch (c) {
		default:
			ls_help();
			return 0;
		}
	}

	if (optind == argc) {
		error = parent_cur(NULL);
		if (error) {
			dbprintf("%s\n", strerror(error));
			exitcode = 1;
		}

		return 0;
	}

	for (c = optind; c < argc; c++) {
		push_cur();

		error = path_walk(argv[c]);
		if (error)
			goto err_cur;

		error = parent_cur(argv[c]);
		if (error)
			goto err_cur;

		pop_cur();
	}

	return 0;
err_cur:
	pop_cur();
	if (error) {
		dbprintf("%s: %s\n", argv[c], strerror(error));
		exitcode = 1;
	}
	return 0;
}

static struct cmdinfo parent_cmd = {
	.name		= "parent",
	.altname	= "pptr",
	.cfunc		= parent_f,
	.argmin		= 0,
	.argmax		= -1,
	.canpush	= 0,
	.args		= "[paths...]",
	.help		= parent_help,
};

void
namei_init(void)
{
	path_cmd.oneline = _("navigate to an inode by path");
	add_command(&path_cmd);

	ls_cmd.oneline = _("list directory contents");
	add_command(&ls_cmd);

	parent_cmd.oneline = _("list parent pointers");
	add_command(&parent_cmd);
}
