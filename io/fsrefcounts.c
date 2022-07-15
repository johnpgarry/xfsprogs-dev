// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "io.h"
#include "input.h"
#include "libfrog/fsgeom.h"

static cmdinfo_t	fsrefcounts_cmd;
static dev_t		xfs_data_dev;

static void
fsrefcounts_help(void)
{
	printf(_(
"\n"
" Prints extent owner counts for the filesystem hosting the current file"
"\n"
" fsrefcounts prints the number of owners of disk blocks used by the whole\n"
" filesystem. When possible, owner and offset information will be included\n"
" in the space report.\n"
"\n"
" By default, each line of the listing takes the following form:\n"
"     extent: major:minor [startblock..endblock]: owner startoffset..endoffset length\n"
" All the file offsets and disk blocks are in units of 512-byte blocks.\n"
" -d -- query only the data device (default).\n"
" -l -- query only the log device.\n"
" -r -- query only the realtime device.\n"
" -n -- query n extents at a time.\n"
" -o -- only print extents with at least this many owners (default 1).\n"
" -O -- only print extents with no more than this many owners (default 2^64-1).\n"
" -m -- output machine-readable format.\n"
" -v -- Verbose information, show AG and offsets.  Show flags legend on 2nd -v\n"
"\n"
"The optional start and end arguments require one of -d, -l, or -r to be set.\n"
"\n"));
}

static void
dump_refcounts(
	unsigned long long		*nr,
	const unsigned long long	min_owners,
	const unsigned long long	max_owners,
	struct xfs_getfsrefs_head	*head)
{
	unsigned long long		i;
	struct xfs_getfsrefs		*p;

	for (i = 0, p = head->fch_recs; i < head->fch_entries; i++, p++) {
		if (p->fcr_owners < min_owners || p->fcr_owners > max_owners)
			continue;
		printf("\t%llu: %u:%u [%lld..%lld]: ", i + (*nr),
			major(p->fcr_device), minor(p->fcr_device),
			(long long)BTOBBT(p->fcr_physical),
			(long long)BTOBBT(p->fcr_physical + p->fcr_length - 1));
		printf(_("%llu %lld\n"),
			(unsigned long long)p->fcr_owners,
			(long long)BTOBBT(p->fcr_length));
	}

	(*nr) += head->fch_entries;
}

static void
dump_refcounts_machine(
	unsigned long long		*nr,
	const unsigned long long	min_owners,
	const unsigned long long	max_owners,
	struct xfs_getfsrefs_head	*head)
{
	unsigned long long		i;
	struct xfs_getfsrefs		*p;

	if (*nr == 0)
		printf(_("EXT,MAJOR,MINOR,PSTART,PEND,OWNERS,LENGTH\n"));
	for (i = 0, p = head->fch_recs; i < head->fch_entries; i++, p++) {
		if (p->fcr_owners < min_owners || p->fcr_owners > max_owners)
			continue;
		printf("%llu,%u,%u,%lld,%lld,", i + (*nr),
			major(p->fcr_device), minor(p->fcr_device),
			(long long)BTOBBT(p->fcr_physical),
			(long long)BTOBBT(p->fcr_physical + p->fcr_length - 1));
		printf("%llu,%lld\n",
			(unsigned long long)p->fcr_owners,
			(long long)BTOBBT(p->fcr_length));
	}

	(*nr) += head->fch_entries;
}

/*
 * Verbose mode displays:
 *   extent: major:minor [startblock..endblock]: owners \
 *	ag# (agoffset..agendoffset) totalbbs flags
 */
#define MINRANGE_WIDTH	16
#define MINAG_WIDTH	2
#define MINTOT_WIDTH	5
#define NFLG		4	/* count of flags */
#define	FLG_NULL	00000	/* Null flag */
#define	FLG_BSU		01000	/* Not on begin of stripe unit  */
#define	FLG_ESU		00100	/* Not on end   of stripe unit  */
#define	FLG_BSW		00010	/* Not on begin of stripe width */
#define	FLG_ESW		00001	/* Not on end   of stripe width */
static void
dump_refcounts_verbose(
	unsigned long long		*nr,
	const unsigned long long	min_owners,
	const unsigned long long	max_owners,
	struct xfs_getfsrefs_head	*head,
	bool				*dumped_flags,
	struct xfs_fsop_geom		*fsgeo)
{
	unsigned long long		i;
	struct xfs_getfsrefs		*p;
	int				agno;
	off64_t				agoff, bperag;
	int				boff_w, aoff_w, tot_w, agno_w, own_w;
	int				nr_w, dev_w;
	char				bbuf[40], abuf[40], obuf[40];
	char				nbuf[40], dbuf[40], gbuf[40];
	int				sunit, swidth;
	int				flg = 0;

	boff_w = aoff_w = own_w = MINRANGE_WIDTH;
	dev_w = 3;
	nr_w = 4;
	tot_w = MINTOT_WIDTH;
	bperag = (off64_t)fsgeo->agblocks *
		  (off64_t)fsgeo->blocksize;
	sunit = (fsgeo->sunit * fsgeo->blocksize);
	swidth = (fsgeo->swidth * fsgeo->blocksize);

	/*
	 * Go through the extents and figure out the width
	 * needed for all columns.
	 */
	for (i = 0, p = head->fch_recs; i < head->fch_entries; i++, p++) {
		if (p->fcr_owners < min_owners || p->fcr_owners > max_owners)
			continue;
		if (sunit &&
		    (p->fcr_physical  % sunit != 0 ||
		     ((p->fcr_physical + p->fcr_length) % sunit) != 0 ||
		     p->fcr_physical % swidth != 0 ||
		     ((p->fcr_physical + p->fcr_length) % swidth) != 0))
			flg = 1;
		if (flg)
			*dumped_flags = true;
		snprintf(nbuf, sizeof(nbuf), "%llu", (*nr) + i);
		nr_w = max(nr_w, strlen(nbuf));
		if (head->fch_oflags & FCH_OF_DEV_T)
			snprintf(dbuf, sizeof(dbuf), "%u:%u",
				major(p->fcr_device),
				minor(p->fcr_device));
		else
			snprintf(dbuf, sizeof(dbuf), "0x%x", p->fcr_device);
		dev_w = max(dev_w, strlen(dbuf));
		snprintf(bbuf, sizeof(bbuf), "[%lld..%lld]:",
			(long long)BTOBBT(p->fcr_physical),
			(long long)BTOBBT(p->fcr_physical + p->fcr_length - 1));
		boff_w = max(boff_w, strlen(bbuf));
		snprintf(obuf, sizeof(obuf), "%llu",
				(unsigned long long)p->fcr_owners);
		own_w = max(own_w, strlen(obuf));
		if (p->fcr_device == xfs_data_dev) {
			agno = p->fcr_physical / bperag;
			agoff = p->fcr_physical - (agno * bperag);
			snprintf(abuf, sizeof(abuf),
				"(%lld..%lld)",
				(long long)BTOBBT(agoff),
				(long long)BTOBBT(agoff + p->fcr_length - 1));
		} else
			abuf[0] = 0;
		aoff_w = max(aoff_w, strlen(abuf));
		tot_w = max(tot_w,
			numlen(BTOBBT(p->fcr_length), 10));
	}
	agno_w = max(MINAG_WIDTH, numlen(fsgeo->agcount, 10));
	if (*nr == 0)
		printf("%*s: %-*s %-*s %-*s %*s %-*s %*s%s\n",
			nr_w, _("EXT"),
			dev_w, _("DEV"),
			boff_w, _("BLOCK-RANGE"),
			own_w, _("OWNERS"),
			agno_w, _("AG"),
			aoff_w, _("AG-OFFSET"),
			tot_w, _("TOTAL"),
			flg ? _(" FLAGS") : "");
	for (i = 0, p = head->fch_recs; i < head->fch_entries; i++, p++) {
		if (p->fcr_owners < min_owners || p->fcr_owners > max_owners)
			continue;
		flg = FLG_NULL;
		/*
		 * If striping enabled, determine if extent starts/ends
		 * on a stripe unit boundary.
		 */
		if (sunit) {
			if (p->fcr_physical  % sunit != 0)
				flg |= FLG_BSU;
			if (((p->fcr_physical +
			      p->fcr_length ) % sunit ) != 0)
				flg |= FLG_ESU;
			if (p->fcr_physical % swidth != 0)
				flg |= FLG_BSW;
			if (((p->fcr_physical +
			      p->fcr_length ) % swidth ) != 0)
				flg |= FLG_ESW;
		}
		if (head->fch_oflags & FCH_OF_DEV_T)
			snprintf(dbuf, sizeof(dbuf), "%u:%u",
				major(p->fcr_device),
				minor(p->fcr_device));
		else
			snprintf(dbuf, sizeof(dbuf), "0x%x", p->fcr_device);
		snprintf(bbuf, sizeof(bbuf), "[%lld..%lld]:",
			(long long)BTOBBT(p->fcr_physical),
			(long long)BTOBBT(p->fcr_physical + p->fcr_length - 1));
		snprintf(obuf, sizeof(obuf), "%llu",
			(unsigned long long)p->fcr_owners);
		if (p->fcr_device == xfs_data_dev) {
			agno = p->fcr_physical / bperag;
			agoff = p->fcr_physical - (agno * bperag);
			snprintf(abuf, sizeof(abuf),
				"(%lld..%lld)",
				(long long)BTOBBT(agoff),
				(long long)BTOBBT(agoff + p->fcr_length - 1));
			snprintf(gbuf, sizeof(gbuf),
				"%lld",
				(long long)agno);
		} else {
			abuf[0] = 0;
			gbuf[0] = 0;
		}
		printf("%*llu: %-*s %-*s %-*s %-*s %-*s %*lld",
			nr_w, (*nr) + i, dev_w, dbuf, boff_w, bbuf, own_w,
			obuf, agno_w, gbuf, aoff_w, abuf, tot_w,
			(long long)BTOBBT(p->fcr_length));
		if (flg == FLG_NULL)
			printf("\n");
		else
			printf(" %-*.*o\n", NFLG, NFLG, flg);
	}

	(*nr) += head->fch_entries;
}

static void
dump_verbose_key(void)
{
	printf(_(" FLAG Values:\n"));
	printf(_("    %*.*o Doesn't begin on stripe unit\n"),
		NFLG+1, NFLG+1, FLG_BSU);
	printf(_("    %*.*o Doesn't end   on stripe unit\n"),
		NFLG+1, NFLG+1, FLG_ESU);
	printf(_("    %*.*o Doesn't begin on stripe width\n"),
		NFLG+1, NFLG+1, FLG_BSW);
	printf(_("    %*.*o Doesn't end   on stripe width\n"),
		NFLG+1, NFLG+1, FLG_ESW);
}

static int
fsrefcounts_f(
	int			argc,
	char			**argv)
{
	struct xfs_getfsrefs		*p;
	struct xfs_getfsrefs_head	*head;
	struct xfs_getfsrefs		*l, *h;
	struct xfs_fsop_geom	fsgeo;
	long long		start = 0;
	long long		end = -1;
	unsigned long long	min_owners = 1;
	unsigned long long	max_owners = ULLONG_MAX;
	int			map_size;
	int			nflag = 0;
	int			vflag = 0;
	int			mflag = 0;
	int			i = 0;
	int			c;
	unsigned long long	nr = 0;
	size_t			fsblocksize, fssectsize;
	struct fs_path		*fs;
	static bool		tab_init;
	bool			dumped_flags = false;
	int			dflag, lflag, rflag;

	init_cvtnum(&fsblocksize, &fssectsize);

	dflag = lflag = rflag = 0;
	while ((c = getopt(argc, argv, "dlmn:o:O:rv")) != EOF) {
		switch (c) {
		case 'd':	/* data device */
			dflag = 1;
			break;
		case 'l':	/* log device */
			lflag = 1;
			break;
		case 'm':	/* machine readable format */
			mflag++;
			break;
		case 'n':	/* number of extents specified */
			nflag = cvt_u32(optarg, 10);
			if (errno)
				return command_usage(&fsrefcounts_cmd);
			break;
		case 'o':	/* minimum owners */
			min_owners = cvt_u64(optarg, 10);
			if (errno)
				return command_usage(&fsrefcounts_cmd);
			if (min_owners < 1) {
				fprintf(stderr,
		_("min_owners must be greater than zero.\n"));
				exitcode = 1;
				return 0;
			}
			break;
		case 'O':	/* maximum owners */
			max_owners = cvt_u64(optarg, 10);
			if (errno)
				return command_usage(&fsrefcounts_cmd);
			if (max_owners < 1) {
				fprintf(stderr,
		_("max_owners must be greater than zero.\n"));
				exitcode = 1;
				return 0;
			}
			break;
		case 'r':	/* rt device */
			rflag = 1;
			break;
		case 'v':	/* Verbose output */
			vflag++;
			break;
		default:
			exitcode = 1;
			return command_usage(&fsrefcounts_cmd);
		}
	}

	if ((dflag + lflag + rflag > 1) || (mflag > 0 && vflag > 0) ||
	    (argc > optind && dflag + lflag + rflag == 0)) {
		exitcode = 1;
		return command_usage(&fsrefcounts_cmd);
	}

	if (argc > optind) {
		start = cvtnum(fsblocksize, fssectsize, argv[optind]);
		if (start < 0) {
			fprintf(stderr,
				_("Bad refcount start_bblock %s.\n"),
				argv[optind]);
			exitcode = 1;
			return 0;
		}
		start <<= BBSHIFT;
	}

	if (argc > optind + 1) {
		end = cvtnum(fsblocksize, fssectsize, argv[optind + 1]);
		if (end < 0) {
			fprintf(stderr,
				_("Bad refcount end_bblock %s.\n"),
				argv[optind + 1]);
			exitcode = 1;
			return 0;
		}
		end <<= BBSHIFT;
	}

	if (vflag) {
		c = -xfrog_geometry(file->fd, &fsgeo);
		if (c) {
			fprintf(stderr,
				_("%s: can't get geometry [\"%s\"]: %s\n"),
				progname, file->name, strerror(c));
			exitcode = 1;
			return 0;
		}
	}

	map_size = nflag ? nflag : 131072 / sizeof(struct xfs_getfsrefs);
	head = malloc(xfs_getfsrefs_sizeof(map_size));
	if (head == NULL) {
		fprintf(stderr, _("%s: malloc of %llu bytes failed.\n"),
				progname,
				(unsigned long long)xfs_getfsrefs_sizeof(map_size));
		exitcode = 1;
		return 0;
	}

	memset(head, 0, sizeof(*head));
	l = head->fch_keys;
	h = head->fch_keys + 1;
	if (dflag) {
		l->fcr_device = h->fcr_device = file->fs_path.fs_datadev;
	} else if (lflag) {
		l->fcr_device = h->fcr_device = file->fs_path.fs_logdev;
	} else if (rflag) {
		l->fcr_device = h->fcr_device = file->fs_path.fs_rtdev;
	} else {
		l->fcr_device = 0;
		h->fcr_device = UINT_MAX;
	}
	l->fcr_physical = start;
	h->fcr_physical = end;
	h->fcr_owners = ULLONG_MAX;
	h->fcr_flags = UINT_MAX;

	/*
	 * If this is an XFS filesystem, remember the data device.
	 * (We report AG number/block for data device extents on XFS).
	 */
	if (!tab_init) {
		fs_table_initialise(0, NULL, 0, NULL);
		tab_init = true;
	}
	fs = fs_table_lookup(file->name, FS_MOUNT_POINT);
	xfs_data_dev = fs ? fs->fs_datadev : 0;

	head->fch_count = map_size;
	do {
		/* Get some extents */
		i = ioctl(file->fd, XFS_IOC_GETFSREFCOUNTS, head);
		if (i < 0) {
			fprintf(stderr, _("%s: xfsctl(XFS_IOC_GETFSREFCOUNTS)"
				" iflags=0x%x [\"%s\"]: %s\n"),
				progname, head->fch_iflags, file->name,
				strerror(errno));
			free(head);
			exitcode = 1;
			return 0;
		}

		if (head->fch_entries == 0)
			break;

		if (vflag)
			dump_refcounts_verbose(&nr, min_owners, max_owners,
					head, &dumped_flags, &fsgeo);
		else if (mflag)
			dump_refcounts_machine(&nr, min_owners, max_owners,
					head);
		else
			dump_refcounts(&nr, min_owners, max_owners, head);

		p = &head->fch_recs[head->fch_entries - 1];
		if (p->fcr_flags & FCR_OF_LAST)
			break;
		xfs_getfsrefs_advance(head);
	} while (true);

	if (dumped_flags)
		dump_verbose_key();

	free(head);
	return 0;
}

void
fsrefcounts_init(void)
{
	fsrefcounts_cmd.name = "fsrefcounts";
	fsrefcounts_cmd.cfunc = fsrefcounts_f;
	fsrefcounts_cmd.argmin = 0;
	fsrefcounts_cmd.argmax = -1;
	fsrefcounts_cmd.flags = CMD_NOMAP_OK | CMD_FLAG_FOREIGN_OK;
	fsrefcounts_cmd.args = _("[-d|-l|-r] [-m|-v] [-n nx] [start] [end]");
	fsrefcounts_cmd.oneline = _("print filesystem owner counts for a range of blocks");
	fsrefcounts_cmd.help = fsrefcounts_help;

	add_command(&fsrefcounts_cmd);
}
