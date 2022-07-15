// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include <sys/stat.h>
#include "libfrog/convert.h"
#include "proto.h"

/*
 * Prototypes for internal functions.
 */
static char *getstr(char **pp);
static void fail(char *msg, int i);
static struct xfs_trans * getres(struct xfs_mount *mp, uint blocks);
static void rsvfile(xfs_mount_t *mp, xfs_inode_t *ip, long long len);
static char *newregfile(char **pp, int *len);
static int metadir_create(struct xfs_mount *mp);
static void rtinit(xfs_mount_t *mp);
static void rtfreesp_init(struct xfs_mount *mp);
static long filesize(int fd);
static int slashes_are_spaces;

/*
 * Use this for block reservations needed for mkfs's conditions
 * (basically no fragmentation).
 */
#define	MKFS_BLOCKRES_INODE	\
	((uint)(M_IGEO(mp)->ialloc_blks + (M_IGEO(mp)->inobt_maxlevels - 1)))
#define	MKFS_BLOCKRES(rb)	\
	((uint)(MKFS_BLOCKRES_INODE + XFS_DA_NODE_MAXDEPTH + \
	(XFS_BM_MAXLEVELS(mp, XFS_DATA_FORK) - 1) + (rb)))

static long long
getnum(
	const char	*str,
	unsigned int	blksize,
	unsigned int	sectsize,
	bool		convert)
{
	long long	i;
	char		*sp;

	if (convert)
		return cvtnum(blksize, sectsize, str);

	i = strtoll(str, &sp, 0);
	if (i == 0 && sp == str)
		return -1LL;
	if (*sp != '\0')
		return -1LL; /* trailing garbage */
	return i;
}

char *
setup_proto(
	char	*fname)
{
	char		*buf = NULL;
	static char	dflt[] = "d--755 0 0 $";
	int		fd;
	long		size;

	if (!fname)
		return dflt;
	if ((fd = open(fname, O_RDONLY)) < 0 || (size = filesize(fd)) < 0) {
		fprintf(stderr, _("%s: failed to open %s: %s\n"),
			progname, fname, strerror(errno));
		goto out_fail;
	}

	buf = malloc(size + 1);
	if (read(fd, buf, size) < size) {
		fprintf(stderr, _("%s: read failed on %s: %s\n"),
			progname, fname, strerror(errno));
		goto out_fail;
	}
	if (buf[size - 1] != '\n') {
		fprintf(stderr, _("%s: proto file %s premature EOF\n"),
			progname, fname);
		goto out_fail;
	}
	buf[size] = '\0';
	/*
	 * Skip past the stuff there for compatibility, a string and 2 numbers.
	 */
	(void)getstr(&buf);	/* boot image name */
	(void)getnum(getstr(&buf), 0, 0, false);	/* block count */
	(void)getnum(getstr(&buf), 0, 0, false);	/* inode count */
	close(fd);
	return buf;

out_fail:
	if (fd >= 0)
		close(fd);
	free(buf);
	exit(1);
}

static void
fail(
	char	*msg,
	int	i)
{
	fprintf(stderr, "%s: %s [%d - %s]\n", progname, msg, i, strerror(i));
	exit(1);
}

void
res_failed(
	int	i)
{
	fail(_("cannot reserve space"), i);
}

static struct xfs_trans *
getres(
	struct xfs_mount *mp,
	uint		blocks)
{
	struct xfs_trans *tp;
	int		i;
	uint		r;

	for (i = 0, r = MKFS_BLOCKRES(blocks); r >= blocks; r--) {
		i = -libxfs_trans_alloc_rollable(mp, r, &tp);
		if (i == 0)
			return tp;
	}
	res_failed(i);
	/* NOTREACHED */
	return NULL;
}

static char *
getstr(
	char	**pp)
{
	char	c;
	char	*p;
	char	*rval;

	p = *pp;
	while ((c = *p)) {
		switch (c) {
		case ' ':
		case '\t':
		case '\n':
			p++;
			continue;
		case ':':
			p++;
			while (*p++ != '\n')
				;
			continue;
		default:
			rval = p;
			while (c != ' ' && c != '\t' && c != '\n' && c != '\0')
				c = *++p;
			*p++ = '\0';
			*pp = p;
			return rval;
		}
	}
	if (c != '\0') {
		fprintf(stderr, _("%s: premature EOF in prototype file\n"),
			progname);
		exit(1);
	}
	return NULL;
}

/* Extract directory entry name from a protofile. */
static char *
getdirentname(
	char	**pp)
{
	char	*p = getstr(pp);
	char	*c = p;

	if (!p)
		return NULL;

	if (!slashes_are_spaces)
		return p;

	/* Replace slash with space because slashes aren't allowed. */
	while (*c) {
		if (*c == '/')
			*c = ' ';
		c++;
	}

	return p;
}

static void
rsvfile(
	xfs_mount_t	*mp,
	xfs_inode_t	*ip,
	long long	llen)
{
	int		error;
	xfs_trans_t	*tp;

	error = -libxfs_alloc_file_space(ip, 0, llen, XFS_BMAPI_PREALLOC);

	if (error) {
		fail(_("error reserving space for a file"), error);
		exit(1);
	}

	/*
	 * update the inode timestamp, mode, and prealloc flag bits
	 */
	error = -libxfs_trans_alloc_rollable(mp, 0, &tp);
	if (error)
		fail(_("allocating transaction for a file"), error);
	libxfs_trans_ijoin(tp, ip, 0);

	VFS_I(ip)->i_mode &= ~S_ISUID;

	/*
	 * Note that we don't have to worry about mandatory
	 * file locking being disabled here because we only
	 * clear the S_ISGID bit if the Group execute bit is
	 * on, but if it was on then mandatory locking wouldn't
	 * have been enabled.
	 */
	if (VFS_I(ip)->i_mode & S_IXGRP)
		VFS_I(ip)->i_mode &= ~S_ISGID;

	libxfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

	ip->i_diflags |= XFS_DIFLAG_PREALLOC;

	libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	error = -libxfs_trans_commit(tp);
	if (error)
		fail(_("committing space for a file failed"), error);
}

static void
writesymlink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	char			*buf,
	int			len)
{
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_extlen_t		nb = XFS_B_TO_FSB(mp, len);
	int			error;

	error = -libxfs_symlink_write_target(tp, ip, buf, len, nb, nb);
	if (error) {
		fprintf(stderr,
	_("%s: error %d creating symlink to '%s'.\n"), progname, error, buf);
		exit(1);
	}
}

static void
writefile(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	char			*buf,
	int			len)
{
	struct xfs_bmbt_irec	map;
	struct xfs_mount	*mp;
	xfs_extlen_t		nb;
	int			nmap;
	int			error;

	mp = ip->i_mount;
	if (len > 0) {
		nb = XFS_B_TO_FSB(mp, len);
		nmap = 1;
		error = -libxfs_bmapi_write(tp, ip, 0, nb, 0, nb, &map, &nmap);
		if (error == ENOSYS && XFS_IS_REALTIME_INODE(ip)) {
			fprintf(stderr,
	_("%s: creating realtime files from proto file not supported.\n"),
					progname);
			exit(1);
		}
		if (error)
			fail(_("error allocating space for a file"), error);
		if (nmap != 1) {
			fprintf(stderr,
				_("%s: cannot allocate space for file\n"),
				progname);
			exit(1);
		}

		error = -libxfs_file_write(tp, ip, buf, len, false);
		if (error)
			fail(_("error writing file"), error);
	}
	ip->i_disk_size = len;
}

static char *
newregfile(
	char		**pp,
	int		*len)
{
	char		*buf;
	int		fd;
	char		*fname;
	long		size;

	fname = getstr(pp);
	if ((fd = open(fname, O_RDONLY)) < 0 || (size = filesize(fd)) < 0) {
		fprintf(stderr, _("%s: cannot open %s: %s\n"),
			progname, fname, strerror(errno));
		exit(1);
	}
	if ((*len = (int)size)) {
		buf = malloc(size);
		if (read(fd, buf, size) < size) {
			fprintf(stderr, _("%s: read failed on %s: %s\n"),
				progname, fname, strerror(errno));
			exit(1);
		}
	} else
		buf = NULL;
	close(fd);
	return buf;
}

static void
newdirent(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_inode	*pip,
	struct xfs_name		*name,
	struct xfs_inode	*ip,
	struct xfs_parent_args	*ppargs)
{
	int	error;
	int	rsv;

	if (!libxfs_dir2_namecheck(name->name, name->len)) {
		fprintf(stderr, _("%.*s: invalid directory entry name\n"),
				name->len, name->name);
		exit(1);
	}

	rsv = XFS_DIRENTER_SPACE_RES(mp, name->len);

	error = -libxfs_dir_createname(tp, pip, name, ip->i_ino, rsv);
	if (error)
		fail(_("directory createname error"), error);

	error = -libxfs_parent_add(tp, ppargs, pip, name, ip);
	if (error)
		fail(_("committing parent pointers failed."), error);
}

static void
newdirectory(
	xfs_mount_t	*mp,
	xfs_trans_t	*tp,
	xfs_inode_t	*dp,
	xfs_inode_t	*pdp)
{
	int	error;

	error = -libxfs_dir_init(tp, dp, pdp);
	if (error)
		fail(_("directory create error"), error);
}

static struct xfs_parent_args *
newpptr(
	struct xfs_mount	*mp)
{
	struct xfs_parent_args	*ret;
	int			error;

	error = -libxfs_parent_start(mp, &ret);
	if (error)
		fail(_("initializing parent pointer"), error);

	return ret;
}

struct cred {
	uid_t		cr_uid;
	gid_t		cr_gid;
};

static int
creatproto(
	struct xfs_trans	**tpp,
	struct xfs_inode	*dp,
	mode_t			mode,
	nlink_t			nlink,
	xfs_dev_t		rdev,
	struct cred		*cr,
	struct fsxattr		*fsx,
	struct xfs_inode	**ipp)
{
	struct xfs_icreate_args	args = {
		.pip		= dp,
		.uid		= make_kuid(cr->cr_uid),
		.gid		= make_kgid(cr->cr_gid),
		.prid		= dp ? libxfs_get_initial_prid(dp) : 0,
		.nlink		= nlink,
		.rdev		= rdev,
		.mode		= mode,
		.flags		= XFS_ICREATE_ARGS_FORCE_UID |
				  XFS_ICREATE_ARGS_FORCE_GID |
				  XFS_ICREATE_ARGS_FORCE_MODE,
	};
	struct xfs_inode	*ip;
	xfs_ino_t		ino;
	int			error;

	if (dp && xfs_has_parent(dp->i_mount))
		args.flags |= XFS_ICREATE_ARGS_INIT_XATTRS;

	/*
	 * Call the space management code to pick the on-disk inode to be
	 * allocated.
	 */
	error = -libxfs_dialloc(tpp, dp, mode, &ino);
	if (error)
		return error;

	error = -libxfs_icreate(*tpp, ino, &args, ipp);
	if (error || dp)
		return error;

	/* If there is no parent dir, initialize the file from fsxattr data. */
	ip = *ipp;
	ip->i_projid = fsx->fsx_projid;
	ip->i_extsize = fsx->fsx_extsize;
	ip->i_diflags = xfs_flags2diflags(ip, fsx->fsx_xflags);

	if (xfs_has_v3inodes(ip->i_mount)) {
		ip->i_diflags2 = xfs_flags2diflags2(ip, fsx->fsx_xflags);
		ip->i_cowextsize = fsx->fsx_cowextsize;
	}
	/* xfsdump breaks if the root dir has a nonzero generation */
	if (!dp)
		VFS_I(ip)->i_generation = 0;
	libxfs_trans_log_inode(*tpp, ip, XFS_ILOG_CORE);
	return 0;
}

static void
parseproto(
	xfs_mount_t	*mp,
	xfs_inode_t	*pip,
	struct fsxattr	*fsxp,
	char		**pp,
	char		*name)
{
#define	IF_REGULAR	0
#define	IF_RESERVED	1
#define	IF_BLOCK	2
#define	IF_CHAR		3
#define	IF_DIRECTORY	4
#define	IF_SYMLINK	5
#define	IF_FIFO		6

	char		*buf;
	int		error;
	int		flags;
	int		fmt;
	int		i;
	xfs_inode_t	*ip;
	int		len;
	long long	llen;
	int		majdev;
	int		mindev;
	int		mode;
	char		*mstr;
	xfs_trans_t	*tp;
	int		val;
	int		isroot = 0;
	struct cred	creds;
	char		*value;
	struct xfs_name	xname;
	struct xfs_parent_args *ppargs = NULL;

	memset(&creds, 0, sizeof(creds));
	mstr = getstr(pp);
	switch (mstr[0]) {
	case '-':
		fmt = IF_REGULAR;
		break;
	case 'r':
		fmt = IF_RESERVED;
		break;
	case 'b':
		fmt = IF_BLOCK;
		break;
	case 'c':
		fmt = IF_CHAR;
		break;
	case 'd':
		fmt = IF_DIRECTORY;
		break;
	case 'l':
		fmt = IF_SYMLINK;
		break;
	case 'p':
		fmt = IF_FIFO;
		break;
	default:
		fprintf(stderr, _("%s: bad format string %s\n"),
			progname, mstr);
		exit(1);
	}
	mode = 0;
	switch (mstr[1]) {
	case '-':
		break;
	case 'u':
		mode |= S_ISUID;
		break;
	default:
		fprintf(stderr, _("%s: bad format string %s\n"),
			progname, mstr);
		exit(1);
	}
	switch (mstr[2]) {
	case '-':
		break;
	case 'g':
		mode |= S_ISGID;
		break;
	default:
		fprintf(stderr, _("%s: bad format string %s\n"),
			progname, mstr);
		exit(1);
	}
	val = 0;
	for (i = 3; i < 6; i++) {
		if (mstr[i] < '0' || mstr[i] > '7') {
			fprintf(stderr, _("%s: bad format string %s\n"),
				progname, mstr);
			exit(1);
		}
		val = val * 8 + mstr[i] - '0';
	}
	mode |= val;
	creds.cr_uid = (int)getnum(getstr(pp), 0, 0, false);
	creds.cr_gid = (int)getnum(getstr(pp), 0, 0, false);
	xname.name = (unsigned char *)name;
	xname.len = name ? strlen(name) : 0;
	xname.type = 0;
	flags = XFS_ILOG_CORE;
	switch (fmt) {
	case IF_REGULAR:
		buf = newregfile(pp, &len);
		tp = getres(mp, XFS_B_TO_FSB(mp, len));
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFREG, 1, 0, &creds,
				fsxp, &ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		writefile(tp, ip, buf, len);
		if (buf)
			free(buf);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_REG_FILE;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		break;

	case IF_RESERVED:			/* pre-allocated space only */
		value = getstr(pp);
		llen = getnum(value, mp->m_sb.sb_blocksize,
			      mp->m_sb.sb_sectsize, true);
		if (llen < 0) {
			fprintf(stderr,
				_("%s: Bad value %s for proto file %s\n"),
				progname, value, name);
			exit(1);
		}
		tp = getres(mp, XFS_B_TO_FSB(mp, llen));
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFREG, 1, 0, &creds,
				fsxp, &ip);
		if (error)
			fail(_("Inode pre-allocation failed"), error);

		libxfs_trans_ijoin(tp, pip, 0);

		xname.type = XFS_DIR3_FT_REG_FILE;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		libxfs_trans_log_inode(tp, ip, flags);
		error = -libxfs_trans_commit(tp);
		if (error)
			fail(_("Space preallocation failed."), error);
		libxfs_parent_finish(mp, ppargs);
		rsvfile(mp, ip, llen);
		libxfs_irele(ip);
		return;

	case IF_BLOCK:
		tp = getres(mp, 0);
		ppargs = newpptr(mp);
		majdev = getnum(getstr(pp), 0, 0, false);
		mindev = getnum(getstr(pp), 0, 0, false);
		error = creatproto(&tp, pip, mode | S_IFBLK, 1,
				IRIX_MKDEV(majdev, mindev), &creds, fsxp, &ip);
		if (error) {
			fail(_("Inode allocation failed"), error);
		}
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_BLKDEV;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		flags |= XFS_ILOG_DEV;
		break;

	case IF_CHAR:
		tp = getres(mp, 0);
		ppargs = newpptr(mp);
		majdev = getnum(getstr(pp), 0, 0, false);
		mindev = getnum(getstr(pp), 0, 0, false);
		error = creatproto(&tp, pip, mode | S_IFCHR, 1,
				IRIX_MKDEV(majdev, mindev), &creds, fsxp, &ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_CHRDEV;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		flags |= XFS_ILOG_DEV;
		break;

	case IF_FIFO:
		tp = getres(mp, 0);
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFIFO, 1, 0, &creds,
				fsxp, &ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_FIFO;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		break;
	case IF_SYMLINK:
		buf = getstr(pp);
		len = (int)strlen(buf);
		tp = getres(mp, XFS_B_TO_FSB(mp, len));
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFLNK, 1, 0, &creds,
				fsxp, &ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		writesymlink(tp, ip, buf, len);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_SYMLINK;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		break;
	case IF_DIRECTORY:
		tp = getres(mp, 0);
		error = creatproto(&tp, pip, mode | S_IFDIR, 1, 0, &creds,
				fsxp, &ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		libxfs_bumplink(tp, ip);		/* account for . */
		if (!pip) {
			pip = ip;
			mp->m_sb.sb_rootino = ip->i_ino;
			libxfs_log_sb(tp);
			isroot = 1;
		} else {
			ppargs = newpptr(mp);
			libxfs_trans_ijoin(tp, pip, 0);
			xname.type = XFS_DIR3_FT_DIR;
			newdirent(mp, tp, pip, &xname, ip, ppargs);
			libxfs_bumplink(tp, pip);
			libxfs_trans_log_inode(tp, pip, XFS_ILOG_CORE);
		}
		newdirectory(mp, tp, ip, pip);
		libxfs_trans_log_inode(tp, ip, flags);
		error = -libxfs_trans_commit(tp);
		if (error)
			fail(_("Directory inode allocation failed."), error);

		libxfs_parent_finish(mp, ppargs);

		/*
		 * RT initialization.  Do this here to ensure that
		 * the RT inodes get placed after the root inode.
		 */
		if (isroot) {
			error = metadir_create(mp);
			if (error)
				fail(
	_("Creation of the metadata directory inode failed"),
					error);

			rtinit(mp);
		}
		tp = NULL;
		for (;;) {
			name = getdirentname(pp);
			if (!name)
				break;
			if (strcmp(name, "$") == 0)
				break;
			parseproto(mp, ip, fsxp, pp, name);
		}
		libxfs_irele(ip);
		return;
	default:
		ASSERT(0);
		fail(_("Unknown format"), EINVAL);
	}
	libxfs_trans_log_inode(tp, ip, flags);
	error = -libxfs_trans_commit(tp);
	if (error) {
		fail(_("Error encountered creating file from prototype file"),
			error);
	}

	libxfs_parent_finish(mp, ppargs);
	libxfs_irele(ip);
}

void
parse_proto(
	xfs_mount_t	*mp,
	struct fsxattr	*fsx,
	char		**pp,
	int		proto_slashes_are_spaces)
{
	slashes_are_spaces = proto_slashes_are_spaces;
	parseproto(mp, NULL, fsx, pp, NULL);
}

/* Create a new metadata root directory. */
static int
metadir_create(
	struct xfs_mount	*mp)
{
	struct xfs_imeta_update	upd;
	struct xfs_inode	*ip = NULL;
	int			error;

	if (!xfs_has_metadir(mp))
		return 0;

	error = -libxfs_imeta_start_create(mp, &XFS_IMETA_METADIR, &upd);
	if (error)
		return error;

	error = -libxfs_imeta_create(&upd, S_IFDIR, &ip);
	if (error)
		goto out_cancel;

	error = -libxfs_imeta_commit_update(&upd);
	if (error)
		goto out_rele;

	mp->m_metadirip = ip;
	return 0;

out_cancel:
	libxfs_imeta_cancel_update(&upd, error);
out_rele:
	if (ip)
		libxfs_irele(ip);
	return error;
}

/* Create the realtime bitmap inode. */
static void
rtbitmap_create(
	struct xfs_mount	*mp)
{
	struct xfs_imeta_update	upd;
	struct xfs_inode	*rbmip;
	int			error;

	error = -libxfs_imeta_ensure_dirpath(mp, &XFS_IMETA_RTBITMAP);
	if (error)
		fail(_("Realtime bitmap directory allocation failed"), error);

	error = -libxfs_imeta_start_create(mp, &XFS_IMETA_RTBITMAP, &upd);
	if (error)
		res_failed(error);

	error = -libxfs_imeta_create(&upd, S_IFREG, &rbmip);
	if (error)
		fail(_("Realtime bitmap inode allocation failed"), error);

	rbmip->i_disk_size = mp->m_sb.sb_rbmblocks * mp->m_sb.sb_blocksize;
	rbmip->i_diflags |= XFS_DIFLAG_NEWRTBM;
	if (!xfs_has_rtgroups(mp))
		inode_set_atime(VFS_I(rbmip), 0, 0);
	libxfs_trans_log_inode(upd.tp, rbmip, XFS_ILOG_CORE);

	error = -libxfs_imeta_commit_update(&upd);
	if (error)
		fail(_("Completion of the realtime bitmap inode failed"),
				error);
	mp->m_rbmip = rbmip;
}

/* Create the realtime summary inode. */
static void
rtsummary_create(
	struct xfs_mount	*mp)
{
	struct xfs_imeta_update	upd;
	struct xfs_inode	*rsumip;
	int			error;

	error = -libxfs_imeta_ensure_dirpath(mp, &XFS_IMETA_RTSUMMARY);
	if (error)
		fail(_("Realtime summary directory allocation failed"), error);

	error = -libxfs_imeta_start_create(mp, &XFS_IMETA_RTSUMMARY, &upd);
	if (error)
		res_failed(error);

	error = -libxfs_imeta_create(&upd, S_IFREG, &rsumip);
	if (error)
		fail(_("Realtime summary inode allocation failed"), error);

	rsumip->i_disk_size = mp->m_rsumsize;
	libxfs_trans_log_inode(upd.tp, rsumip, XFS_ILOG_CORE);

	error = -libxfs_imeta_commit_update(&upd);
	if (error)
		fail(_("Completion of the realtime summary inode failed"),
				error);
	mp->m_rsumip = rsumip;
}

/* Create the realtime rmap btree inode. */
static void
rtrmapbt_create(
	struct xfs_rtgroup	*rtg)
{
	struct xfs_imeta_update	upd;
	struct xfs_rmap_irec	rmap = {
		.rm_startblock	= 0,
		.rm_blockcount	= rtg->rtg_mount->m_sb.sb_rextsize,
		.rm_owner	= XFS_RMAP_OWN_FS,
		.rm_offset	= 0,
		.rm_flags	= 0,
	};
	struct xfs_mount	*mp = rtg->rtg_mount;
	struct xfs_imeta_path	*path;
	struct xfs_btree_cur	*cur;
	int			error;

	error = -libxfs_rtrmapbt_create_path(mp, rtg->rtg_rgno, &path);
	if (error)
		fail( _("rtrmap inode path creation failed"), error);

	error = -libxfs_imeta_ensure_dirpath(mp, path);
	if (error)
		fail(_("rtgroup directory allocation failed"), error);

	error = -libxfs_imeta_start_create(mp, path, &upd);
	if (error)
		res_failed(error);

	error = -libxfs_rtrmapbt_create(&upd, &rtg->rtg_rmapip);
	if (error)
		fail(_("rtrmap inode creation failed"), error);

	/* Adding an rmap for the rtgroup super should fit in the data fork */
	cur = libxfs_rtrmapbt_init_cursor(mp, upd.tp, rtg, rtg->rtg_rmapip);
	error = -libxfs_rmap_map_raw(cur, &rmap);
	libxfs_btree_del_cursor(cur, error);
	if (error)
		fail(_("rtrmapbt initialization failed"), error);

	error = -libxfs_imeta_commit_update(&upd);
	if (error)
		fail(_("rtrmapbt commit failed"), error);

	libxfs_imeta_free_path(path);
}

/* Create the realtime refcount btree inode. */
static void
rtrefcountbt_create(
	struct xfs_rtgroup	*rtg)
{
	struct xfs_imeta_update	upd;
	struct xfs_mount	*mp = rtg->rtg_mount;
	struct xfs_imeta_path	*path;
	int			error;

	error = -libxfs_rtrefcountbt_create_path(mp, rtg->rtg_rgno, &path);
	if (error)
		fail( _("rtrefcount inode path creation failed"), error);

	error = -libxfs_imeta_ensure_dirpath(mp, path);
	if (error)
		fail(_("rtgroup allocation failed"),
				error);

	error = -libxfs_imeta_start_create(mp, path, &upd);
	if (error)
		res_failed(error);

	error = -libxfs_rtrefcountbt_create(&upd, &rtg->rtg_refcountip);
	if (error)
		fail(_("rtrefcount inode creation failed"), error);

	error = -libxfs_imeta_commit_update(&upd);
	if (error)
		fail(_("rtrefcountbt commit failed"), error);

	libxfs_imeta_free_path(path);
}

/* Initialize block headers of rt free space files. */
static int
init_rtblock_headers(
	struct xfs_inode	*ip,
	xfs_fileoff_t		nrblocks,
	const struct xfs_buf_ops *ops,
	uint32_t		magic)
{
	struct xfs_bmbt_irec	map;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_rtbuf_blkinfo *hdr;
	xfs_fileoff_t		off = 0;
	int			error;

	while (off < nrblocks) {
		struct xfs_buf	*bp;
		xfs_daddr_t	daddr;
		int		nimaps = 1;

		error = -libxfs_bmapi_read(ip, off, 1, &map, &nimaps, 0);
		if (error)
			return error;

		daddr = XFS_FSB_TO_DADDR(mp, map.br_startblock);
		error = -libxfs_buf_get(mp->m_ddev_targp, daddr,
				XFS_FSB_TO_BB(mp, map.br_blockcount), &bp);
		if (error)
			return error;

		bp->b_ops = ops;
		hdr = bp->b_addr;
		hdr->rt_magic = cpu_to_be32(magic);
		hdr->rt_owner = cpu_to_be64(ip->i_ino);
		hdr->rt_blkno = cpu_to_be64(daddr);
		platform_uuid_copy(&hdr->rt_uuid, &mp->m_sb.sb_meta_uuid);
		libxfs_buf_mark_dirty(bp);
		libxfs_buf_relse(bp);

		off = map.br_startoff + map.br_blockcount;
	}

	return 0;
}

/* Zero the realtime bitmap. */
static void
rtbitmap_init(
	struct xfs_mount	*mp)
{
	int			error;

	error = -libxfs_alloc_file_space(mp->m_rbmip, 0,
			mp->m_sb.sb_rbmblocks << mp->m_sb.sb_blocklog,
			XFS_BMAPI_ZERO);
	if (error)
		fail(
	_("Block allocation of the realtime bitmap inode failed"),
				error);

	if (xfs_has_rtgroups(mp)) {
		error = init_rtblock_headers(mp->m_rbmip, mp->m_sb.sb_rbmblocks,
				&xfs_rtbitmap_buf_ops, XFS_RTBITMAP_MAGIC);
		if (error)
			fail(_("Initialization of rtbitmap failed"), error);
	}
}

/* Zero the realtime summary file. */
static void
rtsummary_init(
	struct xfs_mount	*mp)
{
	int			error;

	error = -libxfs_alloc_file_space(mp->m_rsumip, 0, mp->m_rsumsize,
			XFS_BMAPI_ZERO);
	if (error)
		fail(
	_("Block allocation of the realtime summary inode failed"),
				error);

	if (xfs_has_rtgroups(mp)) {
		error = init_rtblock_headers(mp->m_rsumip,
				XFS_B_TO_FSB(mp, mp->m_rsumsize),
				&xfs_rtsummary_buf_ops, XFS_RTSUMMARY_MAGIC);
		if (error)
			fail(_("Initialization of rtsummary failed"), error);
	}
}

static void
rtfreesp_init_groups(
	struct xfs_mount	*mp)
{
	xfs_rgnumber_t		rgno;
	int			error;

	for (rgno = 0; rgno < mp->m_sb.sb_rgcount; rgno++) {
		struct xfs_trans	*tp;
		xfs_rtblock_t	rtbno;
		xfs_rtxnum_t	start_rtx;
		xfs_rtxnum_t	next_rtx;

		rtbno = xfs_rgbno_to_rtb(mp, rgno, mp->m_sb.sb_rextsize);
		start_rtx = xfs_rtb_to_rtx(mp, rtbno);

		rtbno = xfs_rgbno_to_rtb(mp, rgno + 1, 0);
		next_rtx = xfs_rtb_to_rtx(mp, rtbno);
		next_rtx = min(next_rtx, mp->m_sb.sb_rextents);

		error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate,
				0, 0, 0, &tp);
		if (error)
			res_failed(error);

		libxfs_trans_ijoin(tp, mp->m_rbmip, 0);
		error = -libxfs_rtfree_extent(tp, start_rtx,
				next_rtx - start_rtx);
		if (error) {
			fail(_("Error initializing the realtime space"),
				error);
		}
		error = -libxfs_trans_commit(tp);
		if (error)
			fail(_("Initialization of the realtime space failed"),
					error);

	}
}

/*
 * Free the whole realtime area using transactions.
 * Do one transaction per bitmap block.
 */
static void
rtfreesp_init(
	struct xfs_mount	*mp)
{
	struct xfs_trans	*tp;
	xfs_rtxnum_t		rtx;
	xfs_rtxnum_t		ertx;
	int			error;

	for (rtx = 0; rtx < mp->m_sb.sb_rextents; rtx = ertx) {
		error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate,
				0, 0, 0, &tp);
		if (error)
			res_failed(error);

		libxfs_trans_ijoin(tp, mp->m_rbmip, 0);
		ertx = XFS_RTMIN(mp->m_sb.sb_rextents,
			rtx + NBBY * mp->m_sb.sb_blocksize);

		error = -libxfs_rtfree_extent(tp, rtx,
				(xfs_rtxlen_t)(ertx - rtx));
		if (error) {
			fail(_("Error initializing the realtime space"),
				error);
		}
		error = -libxfs_trans_commit(tp);
		if (error)
			fail(_("Initialization of the realtime space failed"),
					error);
	}
}

/*
 * Allocate the realtime bitmap and summary inodes, and fill in data if any.
 */
static void
rtinit(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		rgno;

	rtbitmap_create(mp);
	rtsummary_create(mp);

	for_each_rtgroup(mp, rgno, rtg) {
		if (xfs_has_rtrmapbt(mp))
			rtrmapbt_create(rtg);
		if (xfs_has_rtreflink(mp))
			rtrefcountbt_create(rtg);
	}

	if (mp->m_sb.sb_rbmblocks == 0)
		return;

	rtbitmap_init(mp);
	rtsummary_init(mp);
	if (xfs_has_rtgroups(mp))
		rtfreesp_init_groups(mp);
	else
		rtfreesp_init(mp);
}

static long
filesize(
	int		fd)
{
	struct stat	stb;

	if (fstat(fd, &stb) < 0)
		return -1;
	return (long)stb.st_size;
}
