// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2003-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 * Copyright (C) 2015, 2017 Red Hat, Inc.
 * Portions of statx support written by David Howells (dhowells@redhat.com)
 */

#ifdef OVERRIDE_SYSTEM_STATX
#define statx sys_statx
#endif

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "statx.h"
#include "libxfs.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"

#include <fcntl.h>

static cmdinfo_t stat_cmd;
static cmdinfo_t statfs_cmd;
static cmdinfo_t statx_cmd;

off_t
filesize(void)
{
	struct stat	st;

	if (fstat(file->fd, &st) < 0) {
		perror("fstat");
		return -1;
	}
	return st.st_size;
}

static char *
filetype(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFSOCK:
		return _("socket");
	case S_IFDIR:
		return _("directory");
	case S_IFCHR:
		return _("char device");
	case S_IFBLK:
		return _("block device");
	case S_IFREG:
		return _("regular file");
	case S_IFLNK:
		return _("symbolic link");
	case S_IFIFO:
		return _("fifo");
	}
	return NULL;
}

static int
dump_raw_stat(struct stat *st)
{
	printf("stat.blksize = %lu\n", (unsigned long)st->st_blksize);
	printf("stat.nlink = %lu\n", (unsigned long)st->st_nlink);
	printf("stat.uid = %u\n", st->st_uid);
	printf("stat.gid = %u\n", st->st_gid);
	printf("stat.mode: 0%o\n", st->st_mode);
	printf("stat.ino = %llu\n", (unsigned long long)st->st_ino);
	printf("stat.size = %lld\n", (long long)st->st_size);
	printf("stat.blocks = %lld\n", (long long)st->st_blocks);
	printf("stat.atime.tv_sec = %jd\n", (intmax_t)st->st_atim.tv_sec);
	printf("stat.atime.tv_nsec = %ld\n", st->st_atim.tv_nsec);
	printf("stat.ctime.tv_sec = %jd\n", (intmax_t)st->st_ctim.tv_sec);
	printf("stat.ctime.tv_nsec = %ld\n", st->st_ctim.tv_nsec);
	printf("stat.mtime.tv_sec = %jd\n", (intmax_t)st->st_mtim.tv_sec);
	printf("stat.mtime.tv_nsec = %ld\n", st->st_mtim.tv_nsec);
	printf("stat.rdev_major = %u\n", major(st->st_rdev));
	printf("stat.rdev_minor = %u\n", minor(st->st_rdev));
	printf("stat.dev_major = %u\n", major(st->st_dev));
	printf("stat.dev_minor = %u\n", minor(st->st_dev));
	return 0;
}

static void
print_file_info(void)
{
	printf(_("fd.path = \"%s\"\n"), file->name);
	printf(_("fd.flags = %s,%s,%s%s%s%s%s\n"),
		file->flags & IO_OSYNC ? _("sync") : _("non-sync"),
		file->flags & IO_DIRECT ? _("direct") : _("non-direct"),
		file->flags & IO_READONLY ? _("read-only") : _("read-write"),
		file->flags & IO_REALTIME ? _(",real-time") : "",
		file->flags & IO_APPEND ? _(",append-only") : "",
		file->flags & IO_NONBLOCK ? _(",non-block") : "",
		file->flags & IO_TMPFILE ? _(",tmpfile") : "");
}

static void
print_extended_info(int verbose)
{
	struct dioattr dio = {};
	struct fsxattr fsx = {}, fsxa = {};

	if ((ioctl(file->fd, FS_IOC_FSGETXATTR, &fsx)) < 0) {
		if (errno != ENOTTY) {
			perror("FS_IOC_FSGETXATTR");
			exitcode = 1;
		}
		return;
	}

	printf(_("fsxattr.xflags = 0x%x "), fsx.fsx_xflags);
	printxattr(fsx.fsx_xflags, verbose, 0, file->name, 1, 1);
	printf(_("fsxattr.projid = %u\n"), fsx.fsx_projid);
	printf(_("fsxattr.extsize = %u\n"), fsx.fsx_extsize);
	printf(_("fsxattr.cowextsize = %u\n"), fsx.fsx_cowextsize);
	printf(_("fsxattr.nextents = %u\n"), fsx.fsx_nextents);

	/* Only XFS supports FS_IOC_FSGETXATTRA and XFS_IOC_DIOINFO */
	if (file->flags & IO_FOREIGN)
		return;

	if ((ioctl(file->fd, XFS_IOC_FSGETXATTRA, &fsxa)) < 0) {
		perror("XFS_IOC_GETXATTRA");
		exitcode = 1;
		return;
	}

	printf(_("fsxattr.naextents = %u\n"), fsxa.fsx_nextents);

	if ((xfsctl(file->name, file->fd, XFS_IOC_DIOINFO, &dio)) < 0) {
		perror("XFS_IOC_DIOINFO");
		exitcode = 1;
		return;
	}

	printf(_("dioattr.mem = 0x%x\n"), dio.d_mem);
	printf(_("dioattr.miniosz = %u\n"), dio.d_miniosz);
	printf(_("dioattr.maxiosz = %u\n"), dio.d_maxiosz);
}

int
stat_f(
	int		argc,
	char		**argv)
{
	struct stat	st;
	int		c, verbose = 0, raw = 0;

	while ((c = getopt(argc, argv, "rv")) != EOF) {
		switch (c) {
		case 'r':
			raw = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			exitcode = 1;
			return command_usage(&stat_cmd);
		}
	}

	if (fstat(file->fd, &st) < 0) {
		perror("fstat");
		exitcode = 1;
		return 0;
	}

	if (raw)
		return dump_raw_stat(&st);

	print_file_info();

	printf(_("stat.ino = %lld\n"), (long long)st.st_ino);
	printf(_("stat.type = %s\n"), filetype(st.st_mode));
	printf(_("stat.size = %lld\n"), (long long)st.st_size);
	printf(_("stat.blocks = %lld\n"), (long long)st.st_blocks);
	if (verbose) {
		printf(_("stat.atime = %s"), ctime(&st.st_atime));
		printf(_("stat.mtime = %s"), ctime(&st.st_mtime));
		printf(_("stat.ctime = %s"), ctime(&st.st_ctime));
	}

	print_extended_info(verbose);

	return 0;
}

static void
statfs_help(void)
{
        printf(_(
"\n"
" Display file system status.\n"
"\n"
" Options:\n"
" -c -- Print fs summary count data.\n"
" -g -- Print fs geometry data.\n"
" -s -- Print statfs data.\n"
"\n"));
}

#define REPORT_STATFS		(1 << 0)
#define REPORT_GEOMETRY		(1 << 1)
#define REPORT_FSCOUNTS		(1 << 2)

static int
statfs_f(
	int			argc,
	char			**argv)
{
	struct xfs_fsop_counts	fscounts;
	struct xfs_fsop_geom	fsgeo;
	struct statfs		st;
	unsigned int		flags = 0;
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "cgs")) != EOF) {
		switch (c) {
		case 'c':
			flags |= REPORT_FSCOUNTS;
			break;
		case 'g':
			flags |= REPORT_GEOMETRY;
			break;
		case 's':
			flags |= REPORT_STATFS;
			break;
		default:
			exitcode = 1;
			return command_usage(&statfs_cmd);
		}
	}

	if (!flags)
		flags = REPORT_STATFS | REPORT_GEOMETRY | REPORT_FSCOUNTS;

	printf(_("fd.path = \"%s\"\n"), file->name);
	if (flags & REPORT_STATFS) {
		ret = platform_fstatfs(file->fd, &st);
		if (ret < 0) {
			perror("fstatfs");
			exitcode = 1;
		} else {
			printf(_("statfs.f_bsize = %lld\n"),
					(long long) st.f_bsize);
			printf(_("statfs.f_blocks = %lld\n"),
					(long long) st.f_blocks);
			printf(_("statfs.f_bavail = %lld\n"),
					(long long) st.f_bavail);
			printf(_("statfs.f_files = %lld\n"),
					(long long) st.f_files);
			printf(_("statfs.f_ffree = %lld\n"),
					(long long) st.f_ffree);
			printf(_("statfs.f_flags = 0x%llx\n"),
					(long long) st.f_flags);
		}
	}

	if (file->flags & IO_FOREIGN)
		return 0;

	if (flags & REPORT_GEOMETRY) {
		ret = -xfrog_geometry(file->fd, &fsgeo);
		if (ret) {
			xfrog_perror(ret, "XFS_IOC_FSGEOMETRY");
			exitcode = 1;
		} else {
			printf(_("geom.bsize = %u\n"), fsgeo.blocksize);
			printf(_("geom.agcount = %u\n"), fsgeo.agcount);
			printf(_("geom.agblocks = %u\n"), fsgeo.agblocks);
			printf(_("geom.datablocks = %llu\n"),
				(unsigned long long) fsgeo.datablocks);
			printf(_("geom.rtblocks = %llu\n"),
				(unsigned long long) fsgeo.rtblocks);
			printf(_("geom.rtextents = %llu\n"),
				(unsigned long long) fsgeo.rtextents);
			printf(_("geom.rtextsize = %u\n"), fsgeo.rtextsize);
			printf(_("geom.sunit = %u\n"), fsgeo.sunit);
			printf(_("geom.swidth = %u\n"), fsgeo.swidth);
		}
	}

	if (flags & REPORT_FSCOUNTS) {
		ret = ioctl(file->fd, XFS_IOC_FSCOUNTS, &fscounts);
		if (ret < 0) {
			perror("XFS_IOC_FSCOUNTS");
			exitcode = 1;
		} else {
			printf(_("counts.freedata = %llu\n"),
				(unsigned long long) fscounts.freedata);
			printf(_("counts.freertx = %llu\n"),
				(unsigned long long) fscounts.freertx);
			printf(_("counts.freeino = %llu\n"),
				(unsigned long long) fscounts.freeino);
			printf(_("counts.allocino = %llu\n"),
				(unsigned long long) fscounts.allocino);
		}
	}

	return 0;
}

static ssize_t
_statx(
	int		dfd,
	const char	*filename,
	unsigned int	flags,
	unsigned int	mask,
	struct statx	*buffer)
{
#ifdef __NR_statx
	return syscall(__NR_statx, dfd, filename, flags, mask, buffer);
#else
	errno = ENOSYS;
	return -1;
#endif
}

struct statx_masks {
	const char	*name;
	unsigned int	mask;
};

static const struct statx_masks statx_masks[] = {
	{"basic",		STATX_BASIC_STATS},
	{"all",			~STATX__RESERVED},

	{"type",		STATX_TYPE},
	{"mode",		STATX_MODE},
	{"nlink",		STATX_NLINK},
	{"uid",			STATX_UID},
	{"gid",			STATX_GID},
	{"atime",		STATX_ATIME},
	{"mtime",		STATX_MTIME},
	{"ctime",		STATX_CTIME},
	{"ino",			STATX_INO},
	{"size",		STATX_SIZE},
	{"blocks",		STATX_BLOCKS},
	{"btime",		STATX_BTIME},
	{"mnt_id",		STATX_MNT_ID},
	{"dioalign",		STATX_DIOALIGN},
	{"mnt_id_unique",	STATX_MNT_ID_UNIQUE},
	{"subvol",		STATX_SUBVOL},
	{"write_atomic",	STATX_WRITE_ATOMIC},
	{"dio_read_align",	STATX_DIO_READ_ALIGN},
};

static void
statx_help(void)
{
	unsigned int	i;

	printf(_(
"\n"
" Display extended file status.\n"
"\n"
" Options:\n"
" -v -- More verbose output\n"
" -r -- Print raw statx structure fields\n"
" -m mask -- Specify the field mask for the statx call\n"
"            (can also be 'basic' or 'all'; defaults to\n"
"             STATX_BASIC_STATS | STATX_BTIME)\n"
" -m +mask -- Add this to the field mask for the statx call\n"
" -m -mask -- Remove this from the field mask for the statx call\n"
" -D -- Don't sync attributes with the server\n"
" -F -- Force the attributes to be sync'd with the server\n"
"\n"
"statx mask values: "));

	for (i = 0; i < ARRAY_SIZE(statx_masks); i++)
		printf("%s%s", i == 0 ? "" : ", ", statx_masks[i].name);
	printf("\n");
}

/* statx helper */
static int
dump_raw_statx(struct statx *stx)
{
	printf("stat.mask = 0x%x\n", stx->stx_mask);
	printf("stat.blksize = %u\n", stx->stx_blksize);
	printf("stat.attributes = 0x%llx\n", (unsigned long long)stx->stx_attributes);
	printf("stat.nlink = %u\n", stx->stx_nlink);
	printf("stat.uid = %u\n", stx->stx_uid);
	printf("stat.gid = %u\n", stx->stx_gid);
	printf("stat.mode: 0%o\n", stx->stx_mode);
	printf("stat.ino = %llu\n", (unsigned long long)stx->stx_ino);
	printf("stat.size = %llu\n", (unsigned long long)stx->stx_size);
	printf("stat.blocks = %llu\n", (unsigned long long)stx->stx_blocks);
	printf("stat.attributes_mask = 0x%llx\n", (unsigned long long)stx->stx_attributes_mask);
	printf("stat.atime.tv_sec = %lld\n", (long long)stx->stx_atime.tv_sec);
	printf("stat.atime.tv_nsec = %d\n", stx->stx_atime.tv_nsec);
	printf("stat.btime.tv_sec = %lld\n", (long long)stx->stx_btime.tv_sec);
	printf("stat.btime.tv_nsec = %d\n", stx->stx_btime.tv_nsec);
	printf("stat.ctime.tv_sec = %lld\n", (long long)stx->stx_ctime.tv_sec);
	printf("stat.ctime.tv_nsec = %d\n", stx->stx_ctime.tv_nsec);
	printf("stat.mtime.tv_sec = %lld\n", (long long)stx->stx_mtime.tv_sec);
	printf("stat.mtime.tv_nsec = %d\n", stx->stx_mtime.tv_nsec);
	printf("stat.rdev_major = %u\n", stx->stx_rdev_major);
	printf("stat.rdev_minor = %u\n", stx->stx_rdev_minor);
	printf("stat.dev_major = %u\n", stx->stx_dev_major);
	printf("stat.dev_minor = %u\n", stx->stx_dev_minor);
	printf("stat.mnt_id = 0x%llu\n", (unsigned long long)stx->stx_mnt_id);
	printf("stat.dio_mem_align = %u\n", stx->stx_dio_mem_align);
	printf("stat.dio_offset_align = %u\n", stx->stx_dio_offset_align);
	printf("stat.subvol = 0x%llu\n", (unsigned long long)stx->stx_subvol);
	printf("stat.atomic_write_unit_min = %u\n", stx->stx_atomic_write_unit_min);
	printf("stat.atomic_write_unit_max = %u\n", stx->stx_atomic_write_unit_max);
	printf("stat.stx_atomic_write_unit_max_opt = %u\n", stx->stx_atomic_write_unit_max_opt);
	printf("stat.atomic_write_segments_max = %u\n", stx->stx_atomic_write_segments_max);
	printf("stat.dio_read_offset_align = %u\n", stx->stx_dio_read_offset_align);
	return 0;
}

enum statx_mask_op {
	SET,
	REMOVE,
	ADD,
};

static bool
parse_statx_masks(
	char			*optarg,
	unsigned int		*caller_mask)
{
	char			*arg = optarg;
	char			*word;
	unsigned int		i;

	while ((word = strtok(arg, ",")) != NULL) {
		enum statx_mask_op op;
		unsigned int	mask;
		char		*p;

		arg = NULL;

		if (*word == '+') {
			op = ADD;
			word++;
		} else if (*word == '-') {
			op = REMOVE;
			word++;
		} else {
			op = SET;
		}

		for (i = 0; i < ARRAY_SIZE(statx_masks); i++) {
			if (!strcmp(statx_masks[i].name, word)) {
				mask = statx_masks[i].mask;
				goto process_op;
			}
		}

		mask = strtoul(word, &p, 0);
		if (!p || p == word) {
			printf( _("non-numeric mask -- %s\n"), word);
			return false;
		}

process_op:
		switch (op) {
		case ADD:
			*caller_mask |= mask;
			continue;
		case REMOVE:
			*caller_mask &= ~mask;
			continue;
		case SET:
			*caller_mask = mask;
			continue;
		}
	}

	return true;
}

/*
 * options:
 * 	- input flags - query type
 * 	- output style for flags (and all else?) (chars vs. hex?)
 * 	- output - mask out incidental flag or not?
 */
static int
statx_f(
	int		argc,
	char		**argv)
{
	int		c, verbose = 0, raw = 0;
	struct statx	stx;
	int		atflag = 0;
	unsigned int	mask = STATX_BASIC_STATS | STATX_BTIME;

	while ((c = getopt(argc, argv, "m:rvFD")) != EOF) {
		switch (c) {
		case 'm':
			if (!parse_statx_masks(optarg, &mask)) {
				exitcode = 1;
				return 0;
			}
			break;
		case 'r':
			raw = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'F':
			atflag &= ~AT_STATX_SYNC_TYPE;
			atflag |= AT_STATX_FORCE_SYNC;
			break;
		case 'D':
			atflag &= ~AT_STATX_SYNC_TYPE;
			atflag |= AT_STATX_DONT_SYNC;
			break;
		default:
			exitcode = 1;
			return command_usage(&statx_cmd);
		}
	}

	if (raw && verbose)
		return command_usage(&statx_cmd);

	memset(&stx, 0xbf, sizeof(stx));
	if (_statx(file->fd, "", atflag | AT_EMPTY_PATH, mask, &stx) < 0) {
		perror("statx");
		exitcode = 1;
		return 0;
	}
	exitcode = 0;

	if (raw)
		return dump_raw_statx(&stx);

	print_file_info();

	printf(_("stat.ino = %lld\n"), (long long)stx.stx_ino);
	printf(_("stat.type = %s\n"), filetype(stx.stx_mode));
	printf(_("stat.size = %lld\n"), (long long)stx.stx_size);
	printf(_("stat.blocks = %lld\n"), (long long)stx.stx_blocks);
	if (verbose) {
		printf(_("stat.atime = %s"), ctime((time_t *)&stx.stx_atime.tv_sec));
		printf(_("stat.mtime = %s"), ctime((time_t *)&stx.stx_mtime.tv_sec));
		printf(_("stat.ctime = %s"), ctime((time_t *)&stx.stx_ctime.tv_sec));
		if (stx.stx_mask & STATX_BTIME)
			printf(_("stat.btime = %s"),
				ctime((time_t *)&stx.stx_btime.tv_sec));
	}

	print_extended_info(verbose);

	return 0;
}

void
stat_init(void)
{
	stat_cmd.name = "stat";
	stat_cmd.cfunc = stat_f;
	stat_cmd.argmin = 0;
	stat_cmd.argmax = 1;
	stat_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	stat_cmd.args = _("[-v|-r]");
	stat_cmd.oneline = _("statistics on the currently open file");

	statx_cmd.name = "statx";
	statx_cmd.cfunc = statx_f;
	statx_cmd.argmin = 0;
	statx_cmd.argmax = -1;
	statx_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	statx_cmd.args = _("[-v|-r][-m basic | -m all | -m <mask>][-FD]");
	statx_cmd.oneline = _("extended statistics on the currently open file");
	statx_cmd.help = statx_help;

	statfs_cmd.name = "statfs";
	statfs_cmd.cfunc = statfs_f;
	statfs_cmd.argmin = 0;
	statfs_cmd.argmax = -1;
	statfs_cmd.args = _("[-c] [-g] [-s]");
	statfs_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	statfs_cmd.oneline =
		_("statistics on the filesystem of the currently open file");
	statfs_cmd.help = statfs_help;

	add_command(&stat_cmd);
	add_command(&statx_cmd);
	add_command(&statfs_cmd);
}
