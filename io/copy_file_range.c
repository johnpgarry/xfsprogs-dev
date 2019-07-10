// SPDX-License-Identifier: GPL-2.0+
/*
 *  Copyright (c) 2016 Netapp, Inc. All rights reserved.
 */

#include <sys/syscall.h>
#include <sys/uio.h>
#include <xfs/xfs.h>
#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"

static cmdinfo_t copy_range_cmd;

static void
copy_range_help(void)
{
	printf(_("\n\
 Copies a range of bytes from a file into the open file, overwriting any data\n\
 already there.\n\
\n\
 Example:\n\
 'copy_range -s 100 -d 200 -l 300 some_file' - copies 300 bytes from some_file\n\
                                               at offset 100 into the open\n\
					       file at offset 200\n\
 'copy_range some_file' - copies all bytes from some_file into the open file\n\
                          at position 0\n\
 'copy_range -f 2' - copies all bytes from open file 2 into the current open file\n\
                          at position 0\n\
"));
}

/*
 * Issue a raw copy_file_range syscall; for our test program we don't want the
 * glibc buffered copy fallback.
 */
static loff_t
copy_file_range_cmd(int fd, long long *src, long long *dst, size_t len)
{
	loff_t ret;

	do {
		ret = syscall(__NR_copy_file_range, fd, src, file->fd, dst,
				len, 0);
		if (ret == -1) {
			perror("copy_range");
			return errno;
		} else if (ret == 0)
			break;
		len -= ret;
	} while (len > 0);

	return 0;
}

static off64_t
copy_src_filesize(int fd)
{
	struct stat st;

	if (fstat(fd, &st) < 0) {
		perror("fstat");
		return -1;
	};
	return st.st_size;
}

static int
copy_dst_truncate(void)
{
	int ret = ftruncate(file->fd, 0);
	if (ret < 0)
		perror("ftruncate");
	return ret;
}

static int
copy_range_f(int argc, char **argv)
{
	long long src = 0;
	long long dst = 0;
	size_t len = 0;
	int opt;
	int ret;
	int fd;
	int src_file_arg = 1;
	size_t fsblocksize, fssectsize;

	init_cvtnum(&fsblocksize, &fssectsize);

	while ((opt = getopt(argc, argv, "s:d:l:f:")) != -1) {
		switch (opt) {
		case 's':
			src = cvtnum(fsblocksize, fssectsize, optarg);
			if (src < 0) {
				printf(_("invalid source offset -- %s\n"), optarg);
				return 0;
			}
			break;
		case 'd':
			dst = cvtnum(fsblocksize, fssectsize, optarg);
			if (dst < 0) {
				printf(_("invalid destination offset -- %s\n"), optarg);
				return 0;
			}
			break;
		case 'l':
			len = cvtnum(fsblocksize, fssectsize, optarg);
			if (len == -1LL) {
				printf(_("invalid length -- %s\n"), optarg);
				return 0;
			}
			break;
		case 'f':
			fd = atoi(argv[1]);
			if (fd < 0 || fd >= filecount) {
				printf(_("value %d is out of range (0-%d)\n"),
					fd, filecount-1);
				return 0;
			}
			fd = filetable[fd].fd;
			/* Expect no src_file arg */
			src_file_arg = 0;
			break;
		}
	}

	if (optind != argc - src_file_arg)
		return command_usage(&copy_range_cmd);

	if (src_file_arg) {
		fd = openfile(argv[optind], NULL, IO_READONLY, 0, NULL);
		if (fd < 0)
			return 0;
	}

	if (src == 0 && dst == 0 && len == 0) {
		off64_t	sz;

		sz = copy_src_filesize(fd);
		if (sz < 0 || (unsigned long long)sz > SIZE_MAX) {
			ret = 1;
			goto out;
		}
		len = sz;

		ret = copy_dst_truncate();
		if (ret < 0) {
			ret = 1;
			goto out;
		}
	}

	ret = copy_file_range_cmd(fd, &src, &dst, len);
out:
	close(fd);
	return ret;
}

void
copy_range_init(void)
{
	copy_range_cmd.name = "copy_range";
	copy_range_cmd.cfunc = copy_range_f;
	copy_range_cmd.argmin = 1;
	copy_range_cmd.argmax = 7;
	copy_range_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	copy_range_cmd.args = _("[-s src_off] [-d dst_off] [-l len] src_file | -f N");
	copy_range_cmd.oneline = _("Copy a range of data between two files");
	copy_range_cmd.help = copy_range_help;

	add_command(&copy_range_cmd);
}
