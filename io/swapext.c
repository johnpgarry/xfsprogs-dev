// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"
#include "libfrog/file_exchange.h"

static cmdinfo_t swapext_cmd;

static void
swapext_help(void)
{
	printf(_(
"\n"
" Swaps extents between the open file descriptor and the supplied filename.\n"
"\n"
" -a   -- Use atomic extent swapping\n"
" -C   -- Print timing information in a condensed format\n"
" -d N -- Start swapping extents at this offset in the open file\n"
" -e   -- Swap extents to the ends of both files, including the file sizes\n"
" -f   -- Flush changed file data and metadata to disk\n"
" -h   -- Only swap written ranges in the supplied file\n"
" -l N -- Swap this many bytes between the two files\n"
" -n   -- Dry run; do all the parameter validation but do not change anything.\n"
" -s N -- Start swapping extents at this offset in the supplied file\n"
" -t   -- Print timing information\n"
" -u   -- Do not compare the open file's timestamps\n"
" -v   -- 'swapext' for XFS_IOC_SWAPEXT, or 'exchrange' for XFS_IOC_EXCHANGE_RANGE\n"));
}

static void
set_xfd_flags(
	struct xfs_fd	*xfd,
	int		api_ver)
{
	switch (api_ver) {
	case 0:
		xfd->flags |= XFROG_FLAG_FORCE_SWAPEXT;
		break;
	case 1:
		xfd->flags |= XFROG_FLAG_FORCE_EXCH_RANGE;
		break;
	default:
		break;
	}
}

static int
swapext_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct xfs_exch_range	fxr;
	struct stat		stat;
	struct timeval		t1, t2;
	uint64_t		flags = XFS_EXCH_RANGE_NONATOMIC |
					XFS_EXCH_RANGE_FILE2_FRESH |
					XFS_EXCH_RANGE_FULL_FILES;
	int64_t			src_offset = 0;
	int64_t			dest_offset = 0;
	int64_t			length = -1;
	size_t			fsblocksize, fssectsize;
	int			condensed = 0, quiet_flag = 1;
	int			api_ver = -1;
	int			c;
	int			fd;
	int			ret;

	init_cvtnum(&fsblocksize, &fssectsize);
	while ((c = getopt(argc, argv, "Cad:efhl:ns:tuv:")) != -1) {
		switch (c) {
		case 'C':
			condensed = 1;
			break;
		case 'a':
			flags &= ~XFS_EXCH_RANGE_NONATOMIC;
			break;
		case 'd':
			dest_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (dest_offset < 0) {
				printf(
			_("non-numeric open file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~XFS_EXCH_RANGE_FULL_FILES;
			break;
		case 'e':
			flags |= XFS_EXCH_RANGE_TO_EOF;
			flags &= ~XFS_EXCH_RANGE_FULL_FILES;
			break;
		case 'f':
			flags |= XFS_EXCH_RANGE_FSYNC;
			break;
		case 'h':
			flags |= XFS_EXCH_RANGE_FILE1_WRITTEN;
			break;
		case 'l':
			length = cvtnum(fsblocksize, fssectsize, optarg);
			if (length < 0) {
				printf(
			_("non-numeric length argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~XFS_EXCH_RANGE_FULL_FILES;
			break;
		case 'n':
			flags |= XFS_EXCH_RANGE_DRY_RUN;
			break;
		case 's':
			src_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (src_offset < 0) {
				printf(
			_("non-numeric supplied file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~XFS_EXCH_RANGE_FULL_FILES;
			break;
		case 't':
			quiet_flag = 0;
			break;
		case 'u':
			flags &= ~XFS_EXCH_RANGE_FILE2_FRESH;
			break;
		case 'v':
			if (!strcmp(optarg, "swapext"))
				api_ver = 0;
			else if (!strcmp(optarg, "exchrange"))
				api_ver = 1;
			else {
				fprintf(stderr,
			_("version must be 'swapext' or 'exchrange'.\n"));
				return 1;
			}
			break;
		default:
			swapext_help();
			return 0;
		}
	}
	if (optind != argc - 1) {
		swapext_help();
		return 0;
	}

	/* open the donor file */
	fd = openfile(argv[optind], NULL, 0, 0, NULL);
	if (fd < 0)
		return 0;

	ret = -xfd_prepare_geometry(&xfd);
	if (ret) {
		xfrog_perror(ret, "xfd_prepare_geometry");
		exitcode = 1;
		goto out;
	}

	if (length < 0) {
		ret = fstat(file->fd, &stat);
		if (ret) {
			perror("fstat");
			exitcode = 1;
			goto out;
		}

		length = stat.st_size;
	}

	ret = xfrog_file_exchange_prep(&xfd, flags, dest_offset, fd, src_offset,
			length, &fxr);
	if (ret) {
		xfrog_perror(ret, "xfrog_file_exchange_prep");
		exitcode = 1;
		goto out;
	}

	set_xfd_flags(&xfd, api_ver);

	gettimeofday(&t1, NULL);
	ret = xfrog_file_exchange(&xfd, &fxr);
	if (ret) {
		xfrog_perror(ret, "swapext");
		exitcode = 1;
		goto out;
	}
	if (quiet_flag)
		goto out;

	gettimeofday(&t2, NULL);
	t2 = tsub(t2, t1);

	report_io_times("swapext", &t2, dest_offset, length, length, 1,
			condensed);
out:
	close(fd);
	return 0;
}

void
swapext_init(void)
{
	swapext_cmd.name = "swapext";
	swapext_cmd.cfunc = swapext_f;
	swapext_cmd.argmin = 1;
	swapext_cmd.argmax = -1;
	swapext_cmd.flags = CMD_NOMAP_OK;
	swapext_cmd.args = _("[-a] [-e] [-f] [-u] [-d dest_offset] [-s src_offset] [-l length] [-v swapext|exchrange] <donorfile>");
	swapext_cmd.oneline = _("Swap extents between files.");
	swapext_cmd.help = swapext_help;

	add_command(&swapext_cmd);
}
