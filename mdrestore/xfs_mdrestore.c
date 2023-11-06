// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2007 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "xfs_metadump.h"
#include <libfrog/platform.h>

union mdrestore_headers {
	__be32			magic;
	struct xfs_metablock	v1;
};

static struct mdrestore {
	bool	show_progress;
	bool	show_info;
	bool	progress_since_warning;
} mdrestore;

static void
fatal(const char *msg, ...)
{
	va_list		args;

	va_start(args, msg);
	fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, msg, args);
	exit(1);
}

static void
print_progress(const char *fmt, ...)
{
	char		buf[60];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[sizeof(buf)-1] = '\0';

	printf("\r%-59s", buf);
	fflush(stdout);
	mdrestore.progress_since_warning = true;
}

static int
open_device(
	char		*path,
	bool		*is_file)
{
	struct stat	statbuf;
	int		open_flags;
	int		fd;

	open_flags = O_RDWR;
	*is_file = false;

	if (stat(path, &statbuf) < 0)  {
		/* ok, assume it's a file and create it */
		open_flags |= O_CREAT;
		*is_file = true;
	} else if (S_ISREG(statbuf.st_mode))  {
		open_flags |= O_TRUNC;
		*is_file = true;
	} else if (platform_check_ismounted(path, NULL, &statbuf, 0)) {
		/*
		 * check to make sure a filesystem isn't mounted on the device
		 */
		fatal("a filesystem is mounted on target device \"%s\","
				" cannot restore to a mounted filesystem.\n",
				path);
	}

	fd = open(path, open_flags, 0644);
	if (fd < 0)
		fatal("couldn't open \"%s\"\n", path);

	return fd;
}

static void
read_header(
	union mdrestore_headers	*h,
	FILE			*md_fp)
{
	if (fread((uint8_t *)&(h->v1.mb_count),
			sizeof(h->v1) - sizeof(h->magic), 1, md_fp) != 1)
		fatal("error reading from metadump file\n");
}

static void
show_info(
	union mdrestore_headers	*h,
	const char		*md_file)
{
	if (h->v1.mb_info & XFS_METADUMP_INFO_FLAGS) {
		printf("%s: %sobfuscated, %s log, %s metadata blocks\n",
			md_file,
			h->v1.mb_info & XFS_METADUMP_OBFUSCATED ? "":"not ",
			h->v1.mb_info & XFS_METADUMP_DIRTYLOG ? "dirty":"clean",
			h->v1.mb_info & XFS_METADUMP_FULLBLOCKS ? "full":"zeroed");
	} else {
		printf("%s: no informational flags present\n", md_file);
	}
}

/*
 * restore() -- do the actual work to restore the metadump
 *
 * @src_f: A FILE pointer to the source metadump
 * @dst_fd: the file descriptor for the target file
 * @is_target_file: designates whether the target is a regular file
 * @mbp: pointer to metadump's first xfs_metablock, read and verified by the caller
 *
 * src_f should be positioned just past a read the previously validated metablock
 */
static void
restore(
	union mdrestore_headers *h,
	FILE			*md_fp,
	int			ddev_fd,
	int			is_target_file)
{
	struct xfs_metablock	*metablock;	/* header + index + blocks */
	__be64			*block_index;
	char			*block_buffer;
	int			block_size;
	int			max_indices;
	int			cur_index;
	int			mb_count;
	xfs_sb_t		sb;
	int64_t			bytes_read;

	block_size = 1 << h->v1.mb_blocklog;
	max_indices = (block_size - sizeof(xfs_metablock_t)) / sizeof(__be64);

	metablock = (xfs_metablock_t *)calloc(max_indices + 1, block_size);
	if (metablock == NULL)
		fatal("memory allocation failure\n");

	mb_count = be16_to_cpu(h->v1.mb_count);
	if (mb_count == 0 || mb_count > max_indices)
		fatal("bad block count: %u\n", mb_count);

	block_index = (__be64 *)((char *)metablock + sizeof(xfs_metablock_t));
	block_buffer = (char *)metablock + block_size;

	if (fread(block_index, block_size - sizeof(struct xfs_metablock), 1,
			md_fp) != 1)
		fatal("error reading from metadump file\n");

	if (block_index[0] != 0)
		fatal("first block is not the primary superblock\n");

	if (fread(block_buffer, mb_count << h->v1.mb_blocklog, 1, md_fp) != 1)
		fatal("error reading from metadump file\n");

	libxfs_sb_from_disk(&sb, (struct xfs_dsb *)block_buffer);

	if (sb.sb_magicnum != XFS_SB_MAGIC)
		fatal("bad magic number for primary superblock\n");

	/*
	 * Normally the upper bound would be simply XFS_MAX_SECTORSIZE
	 * but the metadump format has a maximum number of BBSIZE blocks
	 * it can store in a single metablock.
	 */
	if (sb.sb_sectsize < XFS_MIN_SECTORSIZE ||
	    sb.sb_sectsize > XFS_MAX_SECTORSIZE ||
	    sb.sb_sectsize > max_indices * block_size)
		fatal("bad sector size %u in metadump image\n", sb.sb_sectsize);

	((struct xfs_dsb*)block_buffer)->sb_inprogress = 1;

	if (is_target_file)  {
		/* ensure regular files are correctly sized */

		if (ftruncate(ddev_fd, sb.sb_dblocks * sb.sb_blocksize))
			fatal("cannot set filesystem image size: %s\n",
				strerror(errno));
	} else  {
		/* ensure device is sufficiently large enough */

		char		lb[XFS_MAX_SECTORSIZE] = { 0 };
		off64_t		off;

		off = sb.sb_dblocks * sb.sb_blocksize - sizeof(lb);
		if (pwrite(ddev_fd, lb, sizeof(lb), off) < 0)
			fatal("failed to write last block, is target too "
				"small? (error: %s)\n", strerror(errno));
	}

	bytes_read = 0;

	for (;;) {
		if (mdrestore.show_progress &&
		    (bytes_read & ((1 << 20) - 1)) == 0)
			print_progress("%lld MB read", bytes_read >> 20);

		for (cur_index = 0; cur_index < mb_count; cur_index++) {
			if (pwrite(ddev_fd, &block_buffer[cur_index <<
					h->v1.mb_blocklog], block_size,
					be64_to_cpu(block_index[cur_index]) <<
						BBSHIFT) < 0)
				fatal("error writing block %llu: %s\n",
					be64_to_cpu(block_index[cur_index]) << BBSHIFT,
					strerror(errno));
		}
		if (mb_count < max_indices)
			break;

		if (fread(metablock, block_size, 1, md_fp) != 1)
			fatal("error reading from metadump file\n");

		mb_count = be16_to_cpu(metablock->mb_count);
		if (mb_count == 0)
			break;
		if (mb_count > max_indices)
			fatal("bad block count: %u\n", mb_count);

		if (fread(block_buffer, mb_count << h->v1.mb_blocklog,
				1, md_fp) != 1)
			fatal("error reading from metadump file\n");

		bytes_read += block_size + (mb_count << h->v1.mb_blocklog);
	}

	if (mdrestore.progress_since_warning)
		putchar('\n');

	memset(block_buffer, 0, sb.sb_sectsize);
	sb.sb_inprogress = 0;
	libxfs_sb_to_disk((struct xfs_dsb *)block_buffer, &sb);
	if (xfs_sb_version_hascrc(&sb)) {
		xfs_update_cksum(block_buffer, sb.sb_sectsize,
				 offsetof(struct xfs_sb, sb_crc));
	}

	if (pwrite(ddev_fd, block_buffer, sb.sb_sectsize, 0) < 0)
		fatal("error writing primary superblock: %s\n", strerror(errno));

	free(metablock);
}

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [-V] [-g] [-i] source target\n", progname);
	exit(1);
}

int
main(
	int			argc,
	char			**argv)
{
	union mdrestore_headers headers;
	FILE			*src_f;
	int			dst_fd;
	int			c;
	bool			is_target_file;

	mdrestore.show_progress = false;
	mdrestore.show_info = false;
	mdrestore.progress_since_warning = false;

	progname = basename(argv[0]);

	while ((c = getopt(argc, argv, "giV")) != EOF) {
		switch (c) {
			case 'g':
				mdrestore.show_progress = true;
				break;
			case 'i':
				mdrestore.show_info = true;
				break;
			case 'V':
				printf("%s version %s\n", progname, VERSION);
				exit(0);
			default:
				usage();
		}
	}

	if (argc - optind < 1 || argc - optind > 2)
		usage();

	/* show_info without a target is ok */
	if (!mdrestore.show_info && argc - optind != 2)
		usage();

	/*
	 * open source and test if this really is a dump. The first metadump
	 * block will be passed to restore() which will continue to read the
	 * file from this point. This avoids rewind the stream, which causes
	 * restore to fail when source was being read from stdin.
 	 */
	if (strcmp(argv[optind], "-") == 0) {
		src_f = stdin;
		if (isatty(fileno(stdin)))
			fatal("cannot read from a terminal\n");
	} else {
		src_f = fopen(argv[optind], "rb");
		if (src_f == NULL)
			fatal("cannot open source dump file\n");
	}

	if (fread(&headers.magic, sizeof(headers.magic), 1, src_f) != 1)
		fatal("Unable to read metadump magic from metadump file\n");

	switch (be32_to_cpu(headers.magic)) {
	case XFS_MD_MAGIC_V1:
		break;
	default:
		fatal("specified file is not a metadata dump\n");
		break;
	}

	read_header(&headers, src_f);

	if (mdrestore.show_info) {
		show_info(&headers, argv[optind]);

		if (argc - optind == 1)
			exit(0);
	}

	optind++;

	/* check and open target */
	dst_fd = open_device(argv[optind], &is_target_file);

	restore(&headers, src_f, dst_fd, is_target_file);

	close(dst_fd);
	if (src_f != stdin)
		fclose(src_f);

	return 0;
}
