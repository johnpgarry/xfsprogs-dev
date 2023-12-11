// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2004 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include "libxfs.h"
#include "libxlog.h"

#include "logprint.h"

#define OP_PRINT	0
#define OP_PRINT_TRANS	1
#define OP_DUMP		2
#define OP_COPY		3

int	print_data;
int	print_only_data;
int	print_inode;
int	print_quota;
int	print_buffer;
int	print_overwrite;
int     print_no_data;
int     print_no_print;
static int	print_operation = OP_PRINT;
static struct libxfs_init x;

static void
usage(void)
{
	fprintf(stderr, _("Usage: %s [options...] <device>\n\n\
Options:\n\
    -c	            try to continue if error found in log\n\
    -C <filename>   copy the log from the filesystem to filename\n\
    -d	            dump the log in log-record format\n\
    -e	            exit when an error is found in the log\n\
    -f	            specified device is actually a file\n\
    -l <device>     filename of external log\n\
    -n	            don't try and interpret log data\n\
    -o	            print buffer data in hex\n\
    -s <start blk>  block # to start printing\n\
    -v              print \"overwrite\" data\n\
    -t	            print out transactional view\n\
	-b          in transactional view, extract buffer info\n\
	-i          in transactional view, extract inode info\n\
	-q          in transactional view, extract quota info\n\
    -D              print only data; no decoding\n\
    -V              print version information\n"),
	progname);
	exit(1);
}

static int
logstat(
	struct xfs_mount	*mp,
	struct xlog		*log)
{
	int		fd;
	char		buf[BBSIZE];

	/* On Linux we always read the superblock of the
	 * filesystem. We need this to get the length of the
	 * log. Otherwise we end up seeking forever. -- mkp
	 */
	if ((fd = open(x.data.name, O_RDONLY)) == -1) {
		fprintf(stderr, _("    Can't open device %s: %s\n"),
			x.data.name, strerror(errno));
		exit(1);
	}
	lseek(fd, 0, SEEK_SET);
	if (read(fd, buf, sizeof(buf)) != sizeof(buf)) {
		fprintf(stderr, _("    read of XFS superblock failed\n"));
		exit(1);
	}
	close (fd);

	if (!x.data.isfile) {
		struct xfs_sb	*sb = &mp->m_sb;

		/*
		 * Conjure up a mount structure
		 */
		libxfs_sb_from_disk(sb, (struct xfs_dsb *)buf);
		mp->m_features |= libxfs_sb_version_to_features(&mp->m_sb);
		mp->m_blkbb_log = sb->sb_blocklog - BBSHIFT;

		xlog_init(mp, log);

		if (!x.log.name && sb->sb_logstart == 0) {
			fprintf(stderr, _("    external log device not specified\n\n"));
			usage();
			/*NOTREACHED*/
		}
	} else {
		struct stat	s;

		stat(x.data.name, &s);

		log->l_logBBsize = s.st_size >> 9;
		log->l_logBBstart = 0;
		log->l_sectBBsize = BTOBB(BBSIZE);
		log->l_dev = mp->m_logdev_targp;
		log->l_mp = mp;
	}

	if (x.log.name && *x.log.name) {    /* External log */
		if ((fd = open(x.log.name, O_RDONLY)) == -1) {
			fprintf(stderr, _("Can't open file %s: %s\n"),
				x.log.name, strerror(errno));
			exit(1);
		}
		close(fd);
	} else {                            /* Internal log */
		x.log.dev = x.data.dev;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int		print_start = -1;
	int		c;
	int             logfd;
	char		*copy_file = NULL;
	struct xlog     log = {0};
	xfs_mount_t	mount;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	memset(&mount, 0, sizeof(mount));
	print_exit = 1; /* -e is now default. specify -c to override */

	progname = basename(argv[0]);
	while ((c = getopt(argc, argv, "bC:cdefl:iqnors:tDVv")) != EOF) {
		switch (c) {
			case 'D':
				print_only_data++;
				print_data++;
				break;
			case 'b':
				print_buffer++;
				break;
			case 'c':
			    /* default is to stop on error.
			     * -c turns this off.
			     */
				print_exit = 0;
				break;
			case 'e':
			    /* -e is now default
			     */
				print_exit = 1;
				break;
			case 'C':
				print_operation = OP_COPY;
				copy_file = optarg;
				break;
			case 'd':
				print_operation = OP_DUMP;
				break;
			case 'f':
				print_skip_uuid++;
				x.data.isfile = 1;
				break;
			case 'l':
				x.log.name = optarg;
				x.log.isfile = 1;
				break;
			case 'i':
				print_inode++;
				break;
			case 'q':
				print_quota++;
				break;
			case 'n':
				print_no_data++;
				break;
			case 'o':
				print_data++;
				break;
			case 's':
				print_start = atoi(optarg);
				break;
			case 't':
				print_operation = OP_PRINT_TRANS;
				break;
			case 'v':
				print_overwrite++;
				break;
			case 'V':
				printf(_("%s version %s\n"), progname, VERSION);
				exit(0);
			default:
				usage();
		}
	}

	if (argc - optind != 1)
		usage();

	x.data.name = argv[optind];

	if (x.data.name == NULL)
		usage();

	x.flags = LIBXFS_ISINACTIVE;
	printf(_("xfs_logprint:\n"));
	if (!libxfs_init(&x))
		exit(1);

	libxfs_buftarg_init(&mount, &x);
	logstat(&mount, &log);

	logfd = (x.log.fd < 0) ? x.data.fd : x.log.fd;

	printf(_("    data device: 0x%llx\n"), (unsigned long long)x.data.dev);

	if (x.log.name) {
		printf(_("    log file: \"%s\" "), x.log.name);
	} else {
		printf(_("    log device: 0x%llx "), (unsigned long long)x.log.dev);
	}

	printf(_("daddr: %lld length: %lld\n\n"),
		(long long)log.l_logBBstart, (long long)log.l_logBBsize);

	ASSERT(x.log.size <= INT_MAX);

	switch (print_operation) {
	case OP_PRINT:
		xfs_log_print(&log, logfd, print_start);
		break;
	case OP_PRINT_TRANS:
		xfs_log_print_trans(&log, print_start);
		break;
	case OP_DUMP:
		xfs_log_dump(&log, logfd, print_start);
		break;
	case OP_COPY:
		xfs_log_copy(&log, logfd, copy_file);
		break;
	}
	exit(0);
}
