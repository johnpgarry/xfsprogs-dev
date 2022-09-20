// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "block.h"
#include "bmap.h"
#include "command.h"
#include "type.h"
#include "faddr.h"
#include "fprint.h"
#include "field.h"
#include "inode.h"
#include "io.h"
#include "output.h"
#include "init.h"

static int	ablock_f(int argc, char **argv);
static void     ablock_help(void);
static int	daddr_f(int argc, char **argv);
static void     daddr_help(void);
static int	dblock_f(int argc, char **argv);
static void     dblock_help(void);
static int	fsblock_f(int argc, char **argv);
static void     fsblock_help(void);
static int	rtblock_f(int argc, char **argv);
static void	rtblock_help(void);
static int	rtextent_f(int argc, char **argv);
static void	rtextent_help(void);
static int	logblock_f(int argc, char **argv);
static void	logblock_help(void);
static void	print_rawdata(void *data, int len);

static const cmdinfo_t	ablock_cmd =
	{ "ablock", NULL, ablock_f, 1, 1, 1, N_("filoff"),
	  N_("set address to file offset (attr fork)"), ablock_help };
static const cmdinfo_t	daddr_cmd =
	{ "daddr", NULL, daddr_f, 0, -1, 1, N_("[d]"),
	  N_("set address to daddr value"), daddr_help };
static const cmdinfo_t	dblock_cmd =
	{ "dblock", NULL, dblock_f, 1, 1, 1, N_("filoff"),
	  N_("set address to file offset (data fork)"), dblock_help };
static const cmdinfo_t	fsblock_cmd =
	{ "fsblock", "fsb", fsblock_f, 0, 1, 1, N_("[fsb]"),
	  N_("set address to fsblock value"), fsblock_help };
static const cmdinfo_t	rtblock_cmd =
	{ "rtblock", "rtbno", rtblock_f, 0, 1, 1, N_("[rtbno]"),
	  N_("set address to rtblock value"), rtblock_help };
static const cmdinfo_t	rtextent_cmd =
	{ "rtextent", "rtx", rtextent_f, 0, 1, 1, N_("[rtxno]"),
	  N_("set address to rtextent value"), rtextent_help };
static const cmdinfo_t	logblock_cmd =
	{ "logblock", "lsb", logblock_f, 0, 1, 1, N_("[logbno]"),
	  N_("set address to logblock value"), logblock_help };

static void
ablock_help(void)
{
	dbprintf(_(
"\n Example:\n"
"\n"
" 'ablock 23' - sets the file position to the 23rd filesystem block in\n"
" the inode's attribute fork.  The filesystem block size is specified in\n"
" the superblock.\n\n"
));
}

/*ARGSUSED*/
static int
ablock_f(
	int			argc,
	char			**argv)
{
	bmap_ext_t		bm;
	xfs_fileoff_t		bno;
	xfs_fsblock_t		dfsbno;
	int			haveattr;
	xfs_extnum_t		nex;
	char			*p;
	struct xfs_dinode	*dip = iocur_top->data;

	bno = (xfs_fileoff_t)strtoull(argv[1], &p, 0);
	if (*p != '\0') {
		dbprintf(_("bad block number %s\n"), argv[1]);
		return 0;
	}
	push_cur();
	set_cur_inode(iocur_top->ino);
	if (!dip) {
		pop_cur();
		dbprintf(_("no current inode\n"));
		return 0;
	}
	haveattr = dip->di_forkoff;
	pop_cur();
	if (!haveattr) {
		dbprintf(_("no attribute data for file\n"));
		return 0;
	}
	nex = 1;
	bmap(bno, 1, XFS_ATTR_FORK, &nex, &bm);
	if (nex == 0) {
		dbprintf(_("file attr block is unmapped\n"));
		return 0;
	}
	dfsbno = bm.startblock + (bno - bm.startoff);
	ASSERT(typtab[TYP_ATTR].typnm == TYP_ATTR);
	set_cur(&typtab[TYP_ATTR], (int64_t)XFS_FSB_TO_DADDR(mp, dfsbno),
		blkbb, DB_RING_ADD, NULL);
	return 0;
}

void
block_init(void)
{
	add_command(&ablock_cmd);
	add_command(&daddr_cmd);
	add_command(&dblock_cmd);
	add_command(&fsblock_cmd);
	add_command(&rtblock_cmd);
	add_command(&rtextent_cmd);
	add_command(&logblock_cmd);
}

static void
daddr_help(void)
{
	dbprintf(_(
"\n Example:\n"
"\n"
" 'daddr 102' - sets position to the 102nd absolute disk block\n"
" (512 byte block).\n"
));
}

enum daddr_target {
	DT_DATA,
	DT_RT,
	DT_LOG,
};

static int
daddr_f(
	int		argc,
	char		**argv)
{
	int64_t		d;
	char		*p;
	int		c;
	xfs_rfsblock_t	max_daddrs = mp->m_sb.sb_dblocks;
	enum daddr_target tgt = DT_DATA;

	while ((c = getopt(argc, argv, "rl")) != -1) {
		switch (c) {
		case 'r':
			tgt = DT_RT;
			max_daddrs = mp->m_sb.sb_rblocks;
			break;
		case 'l':
			tgt = DT_LOG;
			max_daddrs = mp->m_sb.sb_logblocks;
			break;
		default:
			daddr_help();
			return 0;
		}
	}

	if (tgt == DT_LOG && mp->m_sb.sb_logstart > 0) {
		dbprintf(_("filesystem has internal log\n"));
		return 0;
	}

	if (optind == argc) {
		xfs_daddr_t	daddr = iocur_top->off >> BBSHIFT;

		if (iocur_is_ddev(iocur_top))
			dbprintf(_("datadev daddr is %lld\n"), daddr);
		else if (iocur_is_extlogdev(iocur_top))
			dbprintf(_("logdev daddr is %lld\n"), daddr);
		else if (iocur_is_rtdev(iocur_top))
			dbprintf(_("rtdev daddr is %lld\n"), daddr);
		else
			dbprintf(_("current daddr is %lld\n"), daddr);

		return 0;
	}

	if (optind != argc - 1) {
		daddr_help();
		return 0;
	}

	d = (int64_t)strtoull(argv[optind], &p, 0);
	if (*p != '\0' ||
	    d >= max_daddrs << (mp->m_sb.sb_blocklog - BBSHIFT)) {
		dbprintf(_("bad daddr %s\n"), argv[1]);
		return 0;
	}
	ASSERT(typtab[TYP_DATA].typnm == TYP_DATA);
	switch (tgt) {
	case DT_DATA:
		set_cur(&typtab[TYP_DATA], d, 1, DB_RING_ADD, NULL);
		break;
	case DT_RT:
		set_rt_cur(&typtab[TYP_DATA], d, 1, DB_RING_ADD, NULL);
		break;
	case DT_LOG:
		set_log_cur(&typtab[TYP_DATA], d, 1, DB_RING_ADD, NULL);
		break;
	}
	return 0;
}

static void
dblock_help(void)
{
	dbprintf(_(
"\n Example:\n"
"\n"
" 'dblock 23' - sets the file position to the 23rd filesystem block in\n"
" the inode's data fork.  The filesystem block size is specified in the\n"
" superblock.\n\n"
));
}

static inline bool
is_rtfile(
	struct xfs_dinode	*dip)
{
	return dip->di_flags & cpu_to_be16(XFS_DIFLAG_REALTIME);
}

static int
dblock_f(
	int		argc,
	char		**argv)
{
	bbmap_t		bbmap;
	bmap_ext_t	*bmp;
	xfs_fileoff_t	bno;
	xfs_fsblock_t	dfsbno;
	int		nb;
	xfs_extnum_t	nex;
	char		*p;
	typnm_t		type;

	bno = (xfs_fileoff_t)strtoull(argv[1], &p, 0);
	if (*p != '\0') {
		dbprintf(_("bad block number %s\n"), argv[1]);
		return 0;
	}
	push_cur();
	set_cur_inode(iocur_top->ino);
	type = inode_next_type();
	pop_cur();
	if (type == TYP_NONE) {
		dbprintf(_("no type for file data\n"));
		return 0;
	}
	nex = nb = type == TYP_DIR2 ? mp->m_dir_geo->fsbcount : 1;
	bmp = malloc(nb * sizeof(*bmp));
	bmap(bno, nb, XFS_DATA_FORK, &nex, bmp);
	if (nex == 0) {
		dbprintf(_("file data block is unmapped\n"));
		free(bmp);
		return 0;
	}
	dfsbno = bmp->startblock + (bno - bmp->startoff);
	ASSERT(typtab[type].typnm == type);
	if (nex > 1)
		make_bbmap(&bbmap, nex, bmp);
	if (is_rtfile(iocur_top->data))
		set_rt_cur(&typtab[type], (int64_t)XFS_FSB_TO_DADDR(mp, dfsbno),
				nb * blkbb, DB_RING_ADD,
				nex > 1 ? &bbmap : NULL);
	else
		set_cur(&typtab[type], (int64_t)XFS_FSB_TO_DADDR(mp, dfsbno),
				nb * blkbb, DB_RING_ADD,
				nex > 1 ? &bbmap : NULL);
	free(bmp);
	return 0;
}

static void
fsblock_help(void)
{
	dbprintf(_(
"\n Example:\n"
"\n"
" 'fsblock 1023' - sets the file position to the 1023rd filesystem block.\n"
" The filesystem block size is specified in the superblock and set during\n"
" mkfs time.  Offset is absolute (not AG relative).\n\n"
));
}

static int
fsblock_f(
	int		argc,
	char		**argv)
{
	xfs_agblock_t	agbno;
	xfs_agnumber_t	agno;
	xfs_fsblock_t	d;
	char		*p;

	if (argc == 1) {
		if (!iocur_is_ddev(iocur_top)) {
			dbprintf(_("cursor does not point to data device\n"));
			return 0;
		}
		dbprintf(_("current fsblock is %lld\n"),
			XFS_DADDR_TO_FSB(mp, iocur_top->off >> BBSHIFT));
		return 0;
	}
	d = strtoull(argv[1], &p, 0);
	if (*p != '\0') {
		dbprintf(_("bad fsblock %s\n"), argv[1]);
		return 0;
	}
	agno = XFS_FSB_TO_AGNO(mp, d);
	agbno = XFS_FSB_TO_AGBNO(mp, d);
	if (agno >= mp->m_sb.sb_agcount || agbno >= mp->m_sb.sb_agblocks) {
		dbprintf(_("bad fsblock %s\n"), argv[1]);
		return 0;
	}
	ASSERT(typtab[TYP_DATA].typnm == TYP_DATA);
	set_cur(&typtab[TYP_DATA], XFS_AGB_TO_DADDR(mp, agno, agbno),
		blkbb, DB_RING_ADD, NULL);
	return 0;
}

static void
rtblock_help(void)
{
	dbprintf(_(
"\n Example:\n"
"\n"
" 'rtblock 1023' - sets the file position to the 1023rd block on the realtime\n"
" volume. The filesystem block size is specified in the superblock and set\n"
" during mkfs time.\n\n"
));
}

static int
rtblock_f(
	int		argc,
	char		**argv)
{
	xfs_rtblock_t	rtbno;
	char		*p;

	if (argc == 1) {
		if (!iocur_is_rtdev(iocur_top)) {
			dbprintf(_("cursor does not point to rt device\n"));
			return 0;
		}
		dbprintf(_("current rtblock is %lld\n"),
			XFS_BB_TO_FSB(mp, iocur_top->off >> BBSHIFT));
		return 0;
	}
	rtbno = strtoull(argv[1], &p, 0);
	if (*p != '\0') {
		dbprintf(_("bad rtblock %s\n"), argv[1]);
		return 0;
	}
	if (rtbno >= mp->m_sb.sb_rblocks) {
		dbprintf(_("bad rtblock %s\n"), argv[1]);
		return 0;
	}
	ASSERT(typtab[TYP_DATA].typnm == TYP_DATA);
	set_rt_cur(&typtab[TYP_DATA], XFS_FSB_TO_BB(mp, rtbno), blkbb,
			DB_RING_ADD, NULL);
	return 0;
}

static void
rtextent_help(void)
{
	dbprintf(_(
"\n Example:\n"
"\n"
" 'rtextent 10' - sets the file position to the 10th extent on the realtime\n"
" volume. The realtime extent size is specified in the superblock and set\n"
" during mkfs or growfs time.\n\n"
));
}

static int
rtextent_f(
	int		argc,
	char		**argv)
{
	xfs_rtblock_t	rtbno;
	xfs_rtxnum_t	rtx;
	char		*p;

	if (argc == 1) {
		if (!iocur_is_rtdev(iocur_top)) {
			dbprintf(_("cursor does not point to rt device\n"));
			return 0;
		}

		rtbno = XFS_BB_TO_FSB(mp, iocur_top->off >> BBSHIFT);
		dbprintf(_("current rtextent is %lld\n"),
				xfs_rtb_to_rtx(mp, rtbno));
		return 0;
	}
	rtx = strtoull(argv[1], &p, 0);
	if (*p != '\0') {
		dbprintf(_("bad rtextent %s\n"), argv[1]);
		return 0;
	}
	if (rtx >= mp->m_sb.sb_rextents) {
		dbprintf(_("bad rtextent %s\n"), argv[1]);
		return 0;
	}

	rtbno = xfs_rtx_to_rtb(mp, rtx);
	ASSERT(typtab[TYP_DATA].typnm == TYP_DATA);
	set_rt_cur(&typtab[TYP_DATA], XFS_FSB_TO_BB(mp, rtbno),
			mp->m_sb.sb_rextsize * blkbb, DB_RING_ADD, NULL);
	return 0;
}

static void
logblock_help(void)
{
	dbprintf(_(
"\n Example:\n"
"\n"
" 'logblock 1023' - sets the file position to the 1023rd log block.\n"
" The external log device or the block offset within the internal log will be\n"
" chosen as appropriate.\n"
));
}

static int
logblock_f(
	int		argc,
	char		**argv)
{
	xfs_fsblock_t	logblock;
	char		*p;

	if (argc == 1) {
		if (mp->m_sb.sb_logstart > 0 && iocur_is_ddev(iocur_top)) {
			logblock = XFS_DADDR_TO_FSB(mp,
						iocur_top->off >> BBSHIFT);

			if (logblock < mp->m_sb.sb_logstart ||
			    logblock >= mp->m_sb.sb_logstart +
					mp->m_sb.sb_logblocks) {
				dbprintf(
 _("current address not within internal log\n"));
				return 0;
			}

			dbprintf(_("current logblock is %lld\n"),
					logblock - mp->m_sb.sb_logstart);
			return 0;
		}

		if (mp->m_sb.sb_logstart == 0 &&
		    iocur_is_extlogdev(iocur_top)) {
			logblock = XFS_BB_TO_FSB(mp,
						iocur_top->off >> BBSHIFT);

			if (logblock >= mp->m_sb.sb_logblocks) {
				dbprintf(
 _("current address not within external log\n"));
				return 0;
			}

			dbprintf(_("current logblock is %lld\n"), logblock);
			return 0;
		}

		dbprintf(_("current address does not point to log\n"));
		return 0;
	}

	logblock = strtoull(argv[1], &p, 0);
	if (*p != '\0') {
		dbprintf(_("bad logblock %s\n"), argv[1]);
		return 0;
	}

	if (logblock >= mp->m_sb.sb_logblocks) {
		dbprintf(_("bad logblock %s\n"), argv[1]);
		return 0;
	}

	ASSERT(typtab[TYP_DATA].typnm == TYP_DATA);

	if (mp->m_sb.sb_logstart) {
		logblock += mp->m_sb.sb_logstart;
		set_cur(&typtab[TYP_DATA], XFS_FSB_TO_DADDR(mp, logblock),
				blkbb, DB_RING_ADD, NULL);
	} else {
		set_log_cur(&typtab[TYP_DATA], XFS_FSB_TO_BB(mp, logblock),
				blkbb, DB_RING_ADD, NULL);
	}

	return 0;
}

void
print_block(
	const field_t	*fields,
	int		argc,
	char		**argv)
{
	print_rawdata(iocur_top->data, iocur_top->len);
}

static void
print_rawdata(
	void	*data,
	int	len)
{
	int	i;
	int	j;
	int	lastaddr;
	int	offchars;
	unsigned char	*p;

	lastaddr = (len - 1) & ~(32 - 1);
	if (lastaddr < 0x10)
		offchars = 1;
	else if (lastaddr < 0x100)
		offchars = 2;
	else if (lastaddr < 0x1000)
		offchars = 3;
	else
		offchars = 4;
	for (i = 0, p = data; i < len; i += 32) {
		dbprintf("%-0*.*x:", offchars, offchars, i);
		for (j = 0; j < 32 && i + j < len; j++, p++) {
			if ((j & 3) == 0)
				dbprintf(" ");
			dbprintf("%02x", *p);
		}
		dbprintf("\n");
	}
}
