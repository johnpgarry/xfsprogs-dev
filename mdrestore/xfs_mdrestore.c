// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2007 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "xfs_metadump.h"
#include <libfrog/platform.h>

union mdrestore_headers {
	__be32				magic;
	struct xfs_metablock		v1;
	struct xfs_metadump_header	v2;
};

struct mdrestore_ops {
	void (*read_header)(union mdrestore_headers *header, FILE *md_fp);
	void (*show_info)(union mdrestore_headers *header, const char *md_file);
	void (*restore)(union mdrestore_headers *header, FILE *md_fp,
			int ddev_fd, bool is_data_target_file, int logdev_fd,
			bool is_log_target_file);
};

static struct mdrestore {
	struct mdrestore_ops	*mdrops;
	bool			show_progress;
	bool			show_info;
	bool			progress_since_warning;
	bool			external_log;
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
verify_device_size(
	int		dev_fd,
	bool		is_file,
	xfs_rfsblock_t	nr_blocks,
	uint32_t	blocksize)
{
	if (is_file) {
		/* ensure regular files are correctly sized */
		if (ftruncate(dev_fd, nr_blocks * blocksize))
			fatal("cannot set filesystem image size: %s\n",
				strerror(errno));
	} else {
		/* ensure device is sufficiently large enough */
		char		lb[XFS_MAX_SECTORSIZE] = { 0 };
		off64_t		off;

		off = nr_blocks * blocksize - sizeof(lb);
		if (pwrite(dev_fd, lb, sizeof(lb), off) < 0)
			fatal("failed to write last block, is target too "
				"small? (error: %s)\n", strerror(errno));
	}
}

static void
read_header_v1(
	union mdrestore_headers	*h,
	FILE			*md_fp)
{
	if (fread((uint8_t *)&(h->v1.mb_count),
			sizeof(h->v1) - sizeof(h->magic), 1, md_fp) != 1)
		fatal("error reading from metadump file\n");
}

static void
show_info_v1(
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

static void
restore_v1(
	union mdrestore_headers *h,
	FILE			*md_fp,
	int			ddev_fd,
	bool			is_data_target_file,
	int			logdev_fd,
	bool			is_log_target_file)
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

	verify_device_size(ddev_fd, is_data_target_file, sb.sb_dblocks,
			sb.sb_blocksize);

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

static struct mdrestore_ops mdrestore_ops_v1 = {
	.read_header	= read_header_v1,
	.show_info	= show_info_v1,
	.restore	= restore_v1,
};

static void
read_header_v2(
	union mdrestore_headers		*h,
	FILE				*md_fp)
{
	bool				want_external_log;

	if (fread((uint8_t *)&(h->v2) + sizeof(h->v2.xmh_magic),
			sizeof(h->v2) - sizeof(h->v2.xmh_magic), 1, md_fp) != 1)
		fatal("error reading from metadump file\n");

	if (h->v2.xmh_incompat_flags != 0)
		fatal("Metadump header has unknown incompat flags set");

	if (h->v2.xmh_reserved != 0)
		fatal("Metadump header's reserved field has a non-zero value");

	want_external_log = !!(be32_to_cpu(h->v2.xmh_incompat_flags) &
			XFS_MD2_COMPAT_EXTERNALLOG);

	if (want_external_log && !mdrestore.external_log)
		fatal("External Log device is required\n");
}

static void
show_info_v2(
	union mdrestore_headers	*h,
	const char		*md_file)
{
	uint32_t		compat_flags;

	compat_flags = be32_to_cpu(h->v2.xmh_compat_flags);

	printf("%s: %sobfuscated, %s log, external log contents are %sdumped, %s metadata blocks,\n",
		md_file,
		compat_flags & XFS_MD2_COMPAT_OBFUSCATED ? "":"not ",
		compat_flags & XFS_MD2_COMPAT_DIRTYLOG ? "dirty":"clean",
		compat_flags & XFS_MD2_COMPAT_EXTERNALLOG ? "":"not ",
		compat_flags & XFS_MD2_COMPAT_FULLBLOCKS ? "full":"zeroed");
}

#define MDR_IO_BUF_SIZE (8 * 1024 * 1024)

static void
restore_meta_extent(
	FILE		*md_fp,
	int		dev_fd,
	char		*device,
	void		*buf,
	uint64_t	offset,
	int		len)
{
	int		io_size;

	io_size = min(len, MDR_IO_BUF_SIZE);

	do {
		if (fread(buf, io_size, 1, md_fp) != 1)
			fatal("error reading from metadump file\n");
		if (pwrite(dev_fd, buf, io_size, offset) < 0)
			fatal("error writing to %s device at offset %llu: %s\n",
				device, offset, strerror(errno));
		len -= io_size;
		offset += io_size;

		io_size = min(len, io_size);
	} while (len);
}

static void
restore_v2(
	union mdrestore_headers	*h,
	FILE			*md_fp,
	int			ddev_fd,
	bool			is_data_target_file,
	int			logdev_fd,
	bool			is_log_target_file)
{
	struct xfs_sb		sb;
	struct xfs_meta_extent	xme;
	char			*block_buffer;
	int64_t			bytes_read;
	uint64_t		offset;
	int			len;

	block_buffer = malloc(MDR_IO_BUF_SIZE);
	if (block_buffer == NULL)
		fatal("Unable to allocate input buffer memory\n");

	if (fread(&xme, sizeof(xme), 1, md_fp) != 1)
		fatal("error reading from metadump file\n");

	if (xme.xme_addr != 0 || xme.xme_len == 1 ||
	    (be64_to_cpu(xme.xme_addr) & XME_ADDR_DEVICE_MASK) !=
			XME_ADDR_DATA_DEVICE)
		fatal("Invalid superblock disk address/length\n");

	len = BBTOB(be32_to_cpu(xme.xme_len));

	if (fread(block_buffer, len, 1, md_fp) != 1)
		fatal("error reading from metadump file\n");

	libxfs_sb_from_disk(&sb, (struct xfs_dsb *)block_buffer);

	if (sb.sb_magicnum != XFS_SB_MAGIC)
		fatal("bad magic number for primary superblock\n");

	((struct xfs_dsb *)block_buffer)->sb_inprogress = 1;

	verify_device_size(ddev_fd, is_data_target_file, sb.sb_dblocks,
			sb.sb_blocksize);

	if (sb.sb_logstart == 0) {
		ASSERT(mdrestore.external_log == true);
		verify_device_size(logdev_fd, is_log_target_file, sb.sb_logblocks,
				sb.sb_blocksize);
	}

	if (pwrite(ddev_fd, block_buffer, len, 0) < 0)
		fatal("error writing primary superblock: %s\n",
			strerror(errno));

	bytes_read = len;

	do {
		char *device;
		int fd;

		if (fread(&xme, sizeof(xme), 1, md_fp) != 1) {
			if (feof(md_fp))
				break;
			fatal("error reading from metadump file\n");
		}

		offset = BBTOB(be64_to_cpu(xme.xme_addr) & XME_ADDR_DADDR_MASK);
		switch (be64_to_cpu(xme.xme_addr) & XME_ADDR_DEVICE_MASK) {
		case XME_ADDR_DATA_DEVICE:
			device = "data";
			fd = ddev_fd;
			break;
		case XME_ADDR_LOG_DEVICE:
			device = "log";
			fd = logdev_fd;
			break;
		default:
			fatal("Invalid device found in metadump\n");
			break;
		}

		len = BBTOB(be32_to_cpu(xme.xme_len));

		restore_meta_extent(md_fp, fd, device, block_buffer, offset,
				len);

		bytes_read += len;

		if (mdrestore.show_progress) {
			static int64_t mb_read;
			int64_t mb_now = bytes_read >> 20;

			if (mb_now != mb_read) {
				print_progress("%lld MB read", mb_now);
				mb_read = mb_now;
			}
		}
	} while (1);

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
		fatal("error writing primary superblock: %s\n",
			strerror(errno));

	free(block_buffer);

	return;
}

static struct mdrestore_ops mdrestore_ops_v2 = {
	.read_header	= read_header_v2,
	.show_info	= show_info_v2,
	.restore	= restore_v2,
};

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [-V] [-g] [-i] [-l logdev] source target\n",
		progname);
	exit(1);
}

int
main(
	int			argc,
	char			**argv)
{
	union mdrestore_headers	headers;
	FILE			*src_f;
	char			*logdev = NULL;
	int			data_dev_fd;
	int			log_dev_fd;
	int			c;
	bool			is_data_dev_file;
	bool			is_log_dev_file;

	mdrestore.show_progress = false;
	mdrestore.show_info = false;
	mdrestore.progress_since_warning = false;
	mdrestore.external_log = false;

	progname = basename(argv[0]);

	while ((c = getopt(argc, argv, "gil:V")) != EOF) {
		switch (c) {
			case 'g':
				mdrestore.show_progress = true;
				break;
			case 'i':
				mdrestore.show_info = true;
				break;
			case 'l':
				logdev = optarg;
				mdrestore.external_log = true;
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
	 * block will be passed to mdrestore_ops->restore() which will continue
	 * to read the file from this point. This avoids rewind the stream,
	 * which causes restore to fail when source was being read from stdin.
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
		if (logdev != NULL)
			usage();
		mdrestore.mdrops = &mdrestore_ops_v1;
		break;

	case XFS_MD_MAGIC_V2:
		mdrestore.mdrops = &mdrestore_ops_v2;
		break;

	default:
		fatal("specified file is not a metadata dump\n");
		break;
	}

	mdrestore.mdrops->read_header(&headers, src_f);

	if (mdrestore.show_info) {
		mdrestore.mdrops->show_info(&headers, argv[optind]);

		if (argc - optind == 1)
			exit(0);
	}

	optind++;

	/* check and open data device */
	data_dev_fd = open_device(argv[optind], &is_data_dev_file);

	log_dev_fd = -1;
	if (mdrestore.external_log)
		/* check and open log device */
		log_dev_fd = open_device(logdev, &is_log_dev_file);

	mdrestore.mdrops->restore(&headers, src_f, data_dev_fd,
			is_data_dev_file, log_dev_fd, is_log_dev_file);

	close(data_dev_fd);
	if (mdrestore.external_log)
		close(log_dev_fd);

	if (src_f != stdin)
		fclose(src_f);

	return 0;
}
