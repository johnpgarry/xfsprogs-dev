/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <stdio.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include "platform_defs.h"
#include "xfs.h"
#include "input.h"
#include "xfs_scrub.h"
#include "common.h"

/*
 * XFS Online Metadata Scrub (and Repair)
 *
 * The XFS scrubber uses custom XFS ioctls to probe more deeply into the
 * internals of the filesystem.  It takes advantage of scrubbing ioctls
 * to check all the records stored in a metadata object and to
 * cross-reference those records against the other filesystem metadata.
 *
 * After the program gathers command line arguments to figure out
 * exactly what the program is going to do, scrub execution is split up
 * into several separate phases:
 *
 * The "find geometry" phase queries XFS for the filesystem geometry.
 * The block devices for the data, realtime, and log devices are opened.
 * Kernel ioctls are test-queried to see if they actually work (the scrub
 * ioctl in particular), and any other filesystem-specific information
 * is gathered.
 *
 * In the "check internal metadata" phase, we call the metadata scrub
 * ioctl to check the filesystem's internal per-AG btrees.  This
 * includes the AG superblock, AGF, AGFL, and AGI headers, freespace
 * btrees, the regular and free inode btrees, the reverse mapping
 * btrees, and the reference counting btrees.  If the realtime device is
 * enabled, the realtime bitmap and reverse mapping btrees are checked.
 * Quotas, if enabled, are also checked in this phase.
 *
 * Each AG (and the realtime device) has its metadata checked in a
 * separate thread for better performance.  Errors in the internal
 * metadata can be fixed here prior to the inode scan; refer to the
 * section about the "repair filesystem" phase for more information.
 *
 * The "scan all inodes" phase uses BULKSTAT to scan all the inodes in
 * an AG in disk order.  The BULKSTAT information provides enough
 * information to construct a file handle that is used to check the
 * following parts of every file:
 *
 *  - The inode record
 *  - All three block forks (data, attr, CoW)
 *  - If it's a symlink, the symlink target.
 *  - If it's a directory, the directory entries.
 *  - All extended attributes
 *  - The parent pointer
 *
 * Multiple threads are started to check each the inodes of each AG in
 * parallel.  Errors in file metadata can be fixed here; see the section
 * about the "repair filesystem" phase for more information.
 *
 * Next comes the (configurable) "repair filesystem" phase.  The user
 * can instruct this program to fix all problems encountered; to fix
 * only optimality problems and leave the corruptions; or not to touch
 * the filesystem at all.  Any metadata repairs that did not succeed in
 * the previous two phases are retried here; if there are uncorrectable
 * errors, xfs_scrub stops here.
 *
 * The next phase is the "check directory tree" phase.  In this phase,
 * every directory is opened (via file handle) to confirm that each
 * directory is connected to the root.  Directory entries are checked
 * for ambiguous Unicode normalization mappings, which is to say that we
 * look for pairs of entries whose utf-8 strings normalize to the same
 * code point sequence and map to different inodes, because that could
 * be used to trick a user into opening the wrong file.  The names of
 * extended attributes are checked for Unicode normalization collisions.
 *
 * In the "verify data file integrity" phase, we employ GETFSMAP to read
 * the reverse-mappings of all AGs and issue direct-reads of the
 * underlying disk blocks.  We rely on the underlying storage to have
 * checksummed the data blocks appropriately.  Multiple threads are
 * started to check each AG in parallel; a separate thread pool is used
 * to handle the direct reads.
 *
 * In the "check summary counters" phase, use GETFSMAP to tally up the
 * blocks and BULKSTAT to tally up the inodes we saw and compare that to
 * the statfs output.  This gives the user a rough estimate of how
 * thorough the scrub was.
 */

/*
 * Known debug tweaks (pass -d and set the environment variable):
 * XFS_SCRUB_FORCE_ERROR	-- pretend all metadata is corrupt
 * XFS_SCRUB_FORCE_REPAIR	-- repair all metadata even if it's ok
 * XFS_SCRUB_NO_KERNEL		-- pretend there is no kernel ioctl
 * XFS_SCRUB_NO_SCSI_VERIFY	-- disable SCSI VERIFY (if present)
 * XFS_SCRUB_PHASE		-- run only this scrub phase
 * XFS_SCRUB_THREADS		-- start exactly this number of threads
 */

/* Program name; needed for libfrog error reports. */
char				*progname = "xfs_scrub";

/* Debug level; higher values mean more verbosity. */
unsigned int			debug;

/* Display resource usage at the end of each phase? */
static bool			display_rusage;

/* Background mode; higher values insert more pauses between scrub calls. */
unsigned int			bg_mode;

/* Maximum number of processors available to us. */
int				nproc;

/* Number of threads we're allowed to use. */
unsigned int			nr_threads;

/* Verbosity; higher values print more information. */
bool				verbose;

/* Should we scrub the data blocks? */
static bool			scrub_data;

/* Size of a memory page. */
long				page_size;

#define SCRUB_RET_SUCCESS	(0)	/* no problems left behind */
#define SCRUB_RET_CORRUPT	(1)	/* corruption remains on fs */
#define SCRUB_RET_UNOPTIMIZED	(2)	/* fs could be optimized */
#define SCRUB_RET_OPERROR	(4)	/* operational problems */
#define SCRUB_RET_SYNTAX	(8)	/* cmdline args rejected */

static void __attribute__((noreturn))
usage(void)
{
	fprintf(stderr, _("Usage: %s [OPTIONS] mountpoint | device\n"), progname);
	fprintf(stderr, "\n");
	fprintf(stderr, _("Options:\n"));
	fprintf(stderr, _("  -a count     Stop after this many errors are found.\n"));
	fprintf(stderr, _("  -b           Background mode.\n"));
	fprintf(stderr, _("  -e behavior  What to do if errors are found.\n"));
	fprintf(stderr, _("  -m path      Path to /etc/mtab.\n"));
	fprintf(stderr, _("  -n           Dry run.  Do not modify anything.\n"));
	fprintf(stderr, _("  -T           Display timing/usage information.\n"));
	fprintf(stderr, _("  -v           Verbose output.\n"));
	fprintf(stderr, _("  -V           Print version.\n"));
	fprintf(stderr, _("  -x           Scrub file data too.\n"));
	fprintf(stderr, _("  -y           Repair all errors.\n"));

	exit(SCRUB_RET_SYNTAX);
}

int
main(
	int			argc,
	char			**argv)
{
	struct scrub_ctx	ctx = {0};
	char			*mtab = NULL;
	char			*repairstr = "";
	unsigned long long	total_errors;
	bool			moveon = true;
	int			c;
	int			ret = SCRUB_RET_SUCCESS;

	fprintf(stdout, "EXPERIMENTAL xfs_scrub program in use! Use at your own risk!\n");
	return SCRUB_RET_OPERROR;

	progname = basename(argv[0]);
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	pthread_mutex_init(&ctx.lock, NULL);
	ctx.mode = SCRUB_MODE_DEFAULT;
	ctx.error_action = ERRORS_CONTINUE;
	while ((c = getopt(argc, argv, "a:bde:m:nTvxVy")) != EOF) {
		switch (c) {
		case 'a':
			ctx.max_errors = cvt_u64(optarg, 10);
			if (errno) {
				perror(optarg);
				usage();
			}
			break;
		case 'b':
			nr_threads = 1;
			bg_mode++;
			break;
		case 'd':
			debug++;
			break;
		case 'e':
			if (!strcmp("continue", optarg))
				ctx.error_action = ERRORS_CONTINUE;
			else if (!strcmp("shutdown", optarg))
				ctx.error_action = ERRORS_SHUTDOWN;
			else {
				fprintf(stderr,
	_("Unknown error behavior \"%s\".\n"),
						optarg);
				usage();
			}
			break;
		case 'm':
			mtab = optarg;
			break;
		case 'n':
			if (ctx.mode != SCRUB_MODE_DEFAULT) {
				fprintf(stderr,
_("Only one of the options -n or -y may be specified.\n"));
				usage();
			}
			ctx.mode = SCRUB_MODE_DRY_RUN;
			break;
		case 'T':
			display_rusage = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'V':
			fprintf(stdout, _("%s version %s\n"), progname,
					VERSION);
			fflush(stdout);
			return SCRUB_RET_SUCCESS;
		case 'x':
			scrub_data = true;
			break;
		case 'y':
			if (ctx.mode != SCRUB_MODE_DEFAULT) {
				fprintf(stderr,
_("Only one of the options -n or -y may be specified.\n"));
				usage();
			}
			ctx.mode = SCRUB_MODE_REPAIR;
			break;
		case '?':
			/* fall through */
		default:
			usage();
		}
	}

	/* Override thread count if debugger */
	if (debug_tweak_on("XFS_SCRUB_THREADS")) {
		unsigned int	x;

		x = cvt_u32(getenv("XFS_SCRUB_THREADS"), 10);
		if (errno) {
			perror("nr_threads");
			usage();
		}
		nr_threads = x;
	}

	if (optind != argc - 1)
		usage();

	ctx.mntpoint = strdup(argv[optind]);

	/*
	 * If the user did not specify an explicit mount table, try to use
	 * /proc/mounts if it is available, else /etc/mtab.  We prefer
	 * /proc/mounts because it is kernel controlled, while /etc/mtab
	 * may contain garbage that userspace tools like pam_mounts wrote
	 * into it.
	 */
	if (!mtab) {
		if (access(_PATH_PROC_MOUNTS, R_OK) == 0)
			mtab = _PATH_PROC_MOUNTS;
		else
			mtab = _PATH_MOUNTED;
	}

	/* How many CPUs? */
	nproc = sysconf(_SC_NPROCESSORS_ONLN);
	if (nproc < 1)
		nproc = 1;

	/* Set up a page-aligned buffer for read verification. */
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0) {
		str_errno(&ctx, ctx.mntpoint);
		goto out;
	}

	if (debug_tweak_on("XFS_SCRUB_FORCE_REPAIR"))
		ctx.mode = SCRUB_MODE_REPAIR;

	if (xfs_scrub_excessive_errors(&ctx))
		str_info(&ctx, ctx.mntpoint, _("Too many errors; aborting."));

	if (debug_tweak_on("XFS_SCRUB_FORCE_ERROR"))
		str_error(&ctx, ctx.mntpoint, _("Injecting error."));

out:
	total_errors = ctx.errors_found + ctx.runtime_errors;
	if (ctx.need_repair)
		repairstr = _("  Unmount and run xfs_repair.");
	if (total_errors && ctx.warnings_found)
		fprintf(stderr,
_("%s: %llu errors and %llu warnings found.%s\n"),
			ctx.mntpoint, total_errors, ctx.warnings_found,
			repairstr);
	else if (total_errors && ctx.warnings_found == 0)
		fprintf(stderr,
_("%s: %llu errors found.%s\n"),
			ctx.mntpoint, total_errors, repairstr);
	else if (total_errors == 0 && ctx.warnings_found)
		fprintf(stderr,
_("%s: %llu warnings found.\n"),
			ctx.mntpoint, ctx.warnings_found);
	if (ctx.errors_found)
		ret |= SCRUB_RET_CORRUPT;
	if (ctx.warnings_found)
		ret |= SCRUB_RET_UNOPTIMIZED;
	if (ctx.runtime_errors)
		ret |= SCRUB_RET_OPERROR;
	free(ctx.mntpoint);

	return ret;
}
