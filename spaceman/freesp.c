// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2012 Red Hat, Inc.
 * Copyright (c) 2017 Oracle.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include <linux/fiemap.h>
#include <linux/fsmap.h>
#include "libfrog/fsgeom.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "space.h"
#include "input.h"
#include "libfrog/histogram.h"

static int		agcount;
static xfs_agnumber_t	*aglist;
static struct histogram	freesp_hist;
static int		dumpflag;
static long long	equalsize;
static long long	multsize;
static int		seen1;
static int		summaryflag;
static int		gflag;
static bool		rtflag;

static cmdinfo_t freesp_cmd;

static inline void
addhistent(
	long long	h)
{
	int		error;

	error = hist_add_bucket(&freesp_hist, h);
	if (error == EFBIG) {
		printf(_("Too many histogram buckets.\n"));
		return;
	}
	if (error) {
		printf("%s\n", strerror(error));
		return;
	}

	if (h == 0)
		h = 1;
	if (h == 1)
		seen1 = 1;
}

static inline void
addtohist(
	xfs_agnumber_t	agno,
	xfs_agblock_t	agbno,
	off64_t		len)
{
	if (dumpflag)
		printf("%8d %8d %8"PRId64"\n", agno, agbno, len);
	hist_add(&freesp_hist, len);
}

static void
histinit(
	long long	maxlen)
{
	long long	i;

	hist_init(&freesp_hist);
	if (equalsize) {
		for (i = 1; i < maxlen; i += equalsize)
			addhistent(i);
	} else if (multsize) {
		for (i = 1; i < maxlen; i *= multsize)
			addhistent(i);
	} else {
		if (!seen1)
			addhistent(1);
	}
	hist_prepare(&freesp_hist, maxlen);
}

static inline void
printhist(void)
{
	hist_print(&freesp_hist);
}

static int
inaglist(
	xfs_agnumber_t	agno)
{
	int		i;

	if (agcount == 0)
		return 1;
	for (i = 0; i < agcount; i++)
		if (aglist[i] == agno)
			return 1;
	return 0;
}

#define NR_EXTENTS 128

static void
scan_ag(
	xfs_agnumber_t		agno)
{
	struct fsmap_head	*fsmap;
	struct fsmap		*extent;
	struct fsmap		*l, *h;
	struct fsmap		*p;
	struct xfs_fd		*xfd = &file->xfd;
	off64_t			aglen;
	xfs_agblock_t		agbno;
	unsigned long long	freeblks = 0;
	unsigned long long	freeexts = 0;
	int			ret;
	int			i;

	fsmap = malloc(fsmap_sizeof(NR_EXTENTS));
	if (!fsmap) {
		fprintf(stderr, _("%s: fsmap malloc failed.\n"), progname);
		exitcode = 1;
		return;
	}

	memset(fsmap, 0, sizeof(*fsmap));
	fsmap->fmh_count = NR_EXTENTS;
	l = fsmap->fmh_keys;
	h = fsmap->fmh_keys + 1;
	if (agno != NULLAGNUMBER) {
		l->fmr_physical = cvt_agbno_to_b(xfd, agno, 0);
		h->fmr_physical = cvt_agbno_to_b(xfd, agno + 1, 0);
		l->fmr_device = h->fmr_device = file->fs_path.fs_datadev;
	} else {
		l->fmr_physical = 0;
		h->fmr_physical = ULLONG_MAX;
		l->fmr_device = h->fmr_device = file->fs_path.fs_rtdev;
	}
	h->fmr_owner = ULLONG_MAX;
	h->fmr_flags = UINT_MAX;
	h->fmr_offset = ULLONG_MAX;

	while (true) {
		ret = ioctl(file->xfd.fd, FS_IOC_GETFSMAP, fsmap);
		if (ret < 0) {
			fprintf(stderr, _("%s: FS_IOC_GETFSMAP [\"%s\"]: %s\n"),
				progname, file->name, strerror(errno));
			free(fsmap);
			exitcode = 1;
			return;
		}

		/* No more extents to map, exit */
		if (!fsmap->fmh_entries)
			break;

		for (i = 0, extent = fsmap->fmh_recs;
		     i < fsmap->fmh_entries;
		     i++, extent++) {
			if (!(extent->fmr_flags & FMR_OF_SPECIAL_OWNER) ||
			    extent->fmr_owner != XFS_FMR_OWN_FREE)
				continue;
			agbno = cvt_b_to_agbno(xfd, extent->fmr_physical);
			aglen = cvt_b_to_off_fsbt(xfd, extent->fmr_length);
			freeblks += aglen;
			freeexts++;

			addtohist(agno, agbno, aglen);
		}

		p = &fsmap->fmh_recs[fsmap->fmh_entries - 1];
		if (p->fmr_flags & FMR_OF_LAST)
			break;
		fsmap_advance(fsmap);
	}

	if (gflag) {
		if (agno == NULLAGNUMBER)
			printf(_("     rtdev %10llu %10llu\n"), freeexts,
					freeblks);
		else
			printf(_("%10u %10llu %10llu\n"), agno, freeexts,
					freeblks);
	}
	free(fsmap);
}

static void
aglistadd(
	char		*a)
{
	xfs_agnumber_t	x;

	aglist = realloc(aglist, (agcount + 1) * sizeof(*aglist));
	x = cvt_u32(a, 0);
	if (errno) {
		printf(_("Unrecognized AG number: %s\n"), a);
		return;
	}
	aglist[agcount] = x;
	agcount++;
}

static int
init(
	int			argc,
	char			**argv)
{
	struct xfs_fsop_geom	*fsgeom = &file->xfd.fsgeom;
	long long		x;
	int			c;
	int			speced = 0;	/* only one of -b -e -h or -m */

	agcount = dumpflag = equalsize = multsize = optind = gflag = 0;
	seen1 = summaryflag = 0;
	aglist = NULL;
	rtflag = false;

	while ((c = getopt(argc, argv, "a:bde:gh:m:rs")) != EOF) {
		switch (c) {
		case 'a':
			aglistadd(optarg);
			break;
		case 'b':
			if (speced)
				goto many_spec;
			multsize = 2;
			speced = 1;
			break;
		case 'd':
			dumpflag = 1;
			break;
		case 'e':
			if (speced)
				goto many_spec;
			equalsize = cvt_s64(optarg, 0);
			if (errno)
				return command_usage(&freesp_cmd);
			speced = 1;
			break;
		case 'g':
			gflag++;
			break;
		case 'h':
			if (speced && hist_buckets(&freesp_hist) == 0)
				goto many_spec;
			/* addhistent increments histcount */
			x = cvt_s64(optarg, 0);
			if (errno)
				return command_usage(&freesp_cmd);
			addhistent(x);
			speced = 1;
			break;
		case 'm':
			if (speced)
				goto many_spec;
			multsize = cvt_s64(optarg, 0);
			if (errno)
				return command_usage(&freesp_cmd);
			speced = 1;
			break;
		case 'r':
			rtflag = true;
			break;
		case 's':
			summaryflag = 1;
			break;
		default:
			return command_usage(&freesp_cmd);
		}
	}
	if (optind != argc)
		return 0;
	if (!speced)
		multsize = 2;
	histinit(fsgeom->agblocks);
	return 1;
many_spec:
	return command_usage(&freesp_cmd);
}

/*
 * Report on freespace usage in xfs filesystem.
 */
static int
freesp_f(
	int			argc,
	char			**argv)
{
	struct xfs_fsop_geom	*fsgeom = &file->xfd.fsgeom;
	xfs_agnumber_t		agno;

	if (!init(argc, argv))
		return 0;
	if (gflag)
		printf(_("        AG    extents     blocks\n"));
	if (rtflag)
		scan_ag(NULLAGNUMBER);
	for (agno = 0; !rtflag && agno < fsgeom->agcount; agno++) {
		if (inaglist(agno))
			scan_ag(agno);
	}
	if (hist_buckets(&freesp_hist) > 0 && !gflag)
		printhist();
	if (summaryflag)
		hist_summarize(&freesp_hist);
	if (aglist)
		free(aglist);
	hist_free(&freesp_hist);
	return 0;
}

static void
freesp_help(void)
{
	printf(_(
"\n"
"Examine filesystem free space\n"
"\n"
" -a agno  -- Scan only the given AG agno.\n"
" -b       -- binary histogram bin size\n"
" -d       -- debug output\n"
" -e bsize -- Use fixed histogram bin size of bsize\n"
" -g       -- Print only a per-AG summary.\n"
" -h hbsz  -- Use custom histogram bin size of h1.\n"
"             Multiple specifications are allowed.\n"
" -m bmult -- Use histogram bin size multiplier of bmult.\n"
" -r       -- Display realtime device free space information.\n"
" -s       -- Emit freespace summary information.\n"
"\n"
"Only one of -b, -e, -h, or -m may be specified.\n"
"\n"));

}

void
freesp_init(void)
{
	freesp_cmd.name = "freesp";
	freesp_cmd.altname = "fsp";
	freesp_cmd.cfunc = freesp_f;
	freesp_cmd.argmin = 0;
	freesp_cmd.argmax = -1;
	freesp_cmd.args = "[-dgrs] [-a agno]... [ -b | -e bsize | -h h1... | -m bmult ]";
	freesp_cmd.flags = CMD_FLAG_ONESHOT;
	freesp_cmd.oneline = _("Examine filesystem free space");
	freesp_cmd.help = freesp_help;

	add_command(&freesp_cmd);
}

