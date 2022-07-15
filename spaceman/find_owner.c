// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017 Oracle.
 * Copyright (c) 2020 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include <linux/fiemap.h>
#include "libfrog/fsgeom.h"
#include "libfrog/radix-tree.h"
#include "libfrog/paths.h"
#include <linux/fsmap.h>
#include "command.h"
#include "init.h"
#include "space.h"
#include "input.h"
#include "relocation.h"

static cmdinfo_t find_owner_cmd;
static cmdinfo_t resolve_owner_cmd;

#define NR_EXTENTS 128

static void
track_inode_chunks(
	struct xfs_fd	*xfd,
	xfs_agnumber_t	agno,
	uint64_t	physaddr,
	uint64_t	length)
{
	xfs_agblock_t	agbno = cvt_b_to_agbno(xfd, physaddr);
	uint64_t	first_ino = cvt_agino_to_ino(xfd, agno,
						cvt_agbno_to_agino(xfd, agbno));
	uint64_t	num_inodes = cvt_b_to_inode_count(xfd, length);
	uint64_t	i;

	printf(_("AG %d\tInode Range to move: 0x%llx - 0x%llx (length 0x%llx)\n"),
			agno,
			(unsigned long long)first_ino,
			(unsigned long long)first_ino + num_inodes - 1,
			(unsigned long long)length);

	for (i = 0; i < num_inodes; i++)
		set_reloc_iflag(first_ino + i, MOVE_INODE);
}

static void
track_inode(
	struct xfs_fd	*xfd,
	xfs_agnumber_t	agno,
	uint64_t	owner,
	uint64_t	physaddr,
	uint64_t	length)
{
	if (test_reloc_iflag(owner, MOVE_BLOCKS))
		return;

	printf(_("AG %d\tInode 0x%llx: blocks to move to move: 0x%llx - 0x%llx\n"),
			agno,
			(unsigned long long)owner,
			(unsigned long long)physaddr,
			(unsigned long long)physaddr + length - 1);

	set_reloc_iflag(owner, MOVE_BLOCKS);
}

int
find_relocation_targets(
	xfs_agnumber_t		agno)
{
	struct fsmap_head	*fsmap;
	struct fsmap		*extent;
	struct fsmap		*l, *h;
	struct fsmap		*p;
	struct xfs_fd		*xfd = &file->xfd;
	int			ret;
	int			i;

	fsmap = malloc(fsmap_sizeof(NR_EXTENTS));
	if (!fsmap) {
		fprintf(stderr, _("%s: fsmap malloc failed.\n"), progname);
		return -ENOMEM;
	}

	memset(fsmap, 0, sizeof(*fsmap));
	fsmap->fmh_count = NR_EXTENTS;
	l = fsmap->fmh_keys;
	h = fsmap->fmh_keys + 1;
	l->fmr_physical = cvt_agbno_to_b(xfd, agno, 0);
	h->fmr_physical = cvt_agbno_to_b(xfd, agno + 1, 0);
	l->fmr_device = h->fmr_device = file->fs_path.fs_datadev;
	h->fmr_owner = ULLONG_MAX;
	h->fmr_flags = UINT_MAX;
	h->fmr_offset = ULLONG_MAX;

	while (true) {
		printf("Inode count %llu\n", get_reloc_count());
		ret = ioctl(xfd->fd, FS_IOC_GETFSMAP, fsmap);
		if (ret < 0) {
			fprintf(stderr, _("%s: FS_IOC_GETFSMAP [\"%s\"]: %s\n"),
				progname, file->name, strerror(errno));
			free(fsmap);
			return -errno;
		}

		/* No more extents to map, exit */
		if (!fsmap->fmh_entries)
			break;

		/*
		 * Walk the extents, ignore everything except inode chunks
		 * and inode owned blocks.
		 */
		for (i = 0, extent = fsmap->fmh_recs;
		     i < fsmap->fmh_entries;
		     i++, extent++) {
			if (extent->fmr_flags & FMR_OF_SPECIAL_OWNER) {
				if (extent->fmr_owner != XFS_FMR_OWN_INODES)
					continue;
				/*
				 * This extent contains inodes that need to be
				 * moved into another AG. Convert the extent to
				 * a range of inode numbers and track them all.
				 */
				track_inode_chunks(xfd, agno,
							extent->fmr_physical,
							extent->fmr_length);

				continue;
			}

			/*
			 * Extent is owned by an inode that may be located
			 * anywhere in the filesystem, not just this AG.
			 */
			track_inode(xfd, agno, extent->fmr_owner,
					extent->fmr_physical,
					extent->fmr_length);
		}

		p = &fsmap->fmh_recs[fsmap->fmh_entries - 1];
		if (p->fmr_flags & FMR_OF_LAST)
			break;
		fsmap_advance(fsmap);
	}

	free(fsmap);
	return 0;
}

/*
 * find inodes that own physical space in a given AG.
 */
static int
find_owner_f(
	int			argc,
	char			**argv)
{
	xfs_agnumber_t		agno = -1;
	int			ret;
	int			c;

	while ((c = getopt(argc, argv, "a:")) != EOF) {
		switch (c) {
		case 'a':
			agno = cvt_u32(optarg, 10);
			if (errno) {
				fprintf(stderr, _("bad agno value %s\n"),
					optarg);
				return command_usage(&find_owner_cmd);
			}
			break;
		default:
			return command_usage(&find_owner_cmd);
		}
	}

	if (optind != argc)
		return command_usage(&find_owner_cmd);

	if (agno == -1 || agno >= file->xfd.fsgeom.agcount) {
		fprintf(stderr,
_("Destination AG %d does not exist. Filesystem only has %d AGs\n"),
			agno, file->xfd.fsgeom.agcount);
		exitcode = 1;
		return 0;
	}

	/*
	 * Check that rmap is enabled so that GETFSMAP is actually useful.
	 */
	if (!(file->xfd.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_RMAPBT)) {
		fprintf(stderr,
_("Filesystem at %s does not have reverse mapping enabled. Aborting.\n"),
			file->fs_path.fs_dir);
		exitcode = 1;
		return 0;
	}

	ret = find_relocation_targets(agno);
	if (ret)
		exitcode = 1;
	return 0;
}

static void
find_owner_help(void)
{
	printf(_(
"\n"
"Find inodes owning physical blocks in a given AG.\n"
"\n"
" -a agno  -- Scan the given AG agno.\n"
"\n"));

}

void
find_owner_init(void)
{
	find_owner_cmd.name = "find_owner";
	find_owner_cmd.altname = "fown";
	find_owner_cmd.cfunc = find_owner_f;
	find_owner_cmd.argmin = 2;
	find_owner_cmd.argmax = 2;
	find_owner_cmd.args = "-a agno";
	find_owner_cmd.flags = CMD_FLAG_ONESHOT;
	find_owner_cmd.oneline = _("Find inodes owning physical blocks in a given AG");
	find_owner_cmd.help = find_owner_help;

	add_command(&find_owner_cmd);
}

static int
resolve_owner_cb(
	const char		*path,
	const struct stat	*stat,
	int			status,
	struct FTW		*data)
{
	struct inode_path	*ipath, *slot_ipath;
	struct inode_path	**slot;

	/*
	 * Lookup the slot rather than the entry so we can replace the contents
	 * without another lookup later on.
	 */
	slot = get_reloc_ipath_slot(stat->st_ino);
	if (!slot)
		return 0;

	/* Could not get stat data? Fail! */
	if (status == FTW_NS) {
		fprintf(stderr,
_("Failed to obtain stat(2) information from path %s. Aborting\n"),
			path);
		return -EPERM;
	}

	/* Allocate a new inode path and record the path in it. */
	ipath = ipath_alloc(path, stat);
	if (!ipath)
		return -ENOMEM;

	/*
	 * If the slot contains the inode number we just looked up, then we
	 * haven't recorded a path for it yet. If that is the case, we just
	 * set the link count of the path to 1 and replace the slot contents
	 * with our new_ipath.
	 */
	if (*slot == UNLINKED_IPATH) {
		ipath->link_count = 1;
		*slot = ipath;
		set_reloc_iflag(stat->st_ino, INODE_PATH);
		return 0;
	}

	/*
	 * Multiple hard links to this inode. The slot already contains an
	 * ipath pointer, so we add the new ipath to the tail of the list held
	 * by the slot's ipath and bump the link count of the slot's ipath to
	 * keep track of how many hard links the inode has.
	 */
	slot_ipath = *slot;
	slot_ipath->link_count++;
	list_add_tail(&ipath->path_list, &slot_ipath->path_list);
	return 0;
}

/*
 * This should be parallelised - pass subdirs off to a work queue, have the
 * work queue processes subdirs, queueing more subdirs to work on.
 */
int
resolve_target_paths(
	const char	*mntpt)
{
	int		ret;

	ret = nftw(mntpt, resolve_owner_cb,
                        100, FTW_PHYS | FTW_MOUNT | FTW_DEPTH);
	if (ret)
		return -errno;
	return 0;
}

static int
list_inode_paths(void)
{
	struct inode_path	*ipath;
	uint64_t		idx = 0;
	int			ret;

	do {
		bool		move_blocks;
		bool		move_inode;

		ipath = get_next_reloc_ipath(idx);
		if (!ipath)
			break;
		idx = ipath->ino + 1;

		/* Grab status tags and remove from tree. */
		move_blocks = test_reloc_iflag(ipath->ino, MOVE_BLOCKS);
		move_inode = test_reloc_iflag(ipath->ino, MOVE_INODE);
		forget_reloc_ino(ipath->ino);

		/* Print the initial path with inode number and state. */
		printf("0x%.16llx\t%s\t%s\t%8d\t%s\n",
				(unsigned long long)ipath->ino,
				move_blocks ? "BLOCK" : "---",
				move_inode ? "INODE" : "---",
				ipath->link_count, ipath->path);
		ipath->link_count--;

		/* Walk all the hard link paths and emit them. */
		while (!list_empty(&ipath->path_list)) {
			struct inode_path	*hpath;

			hpath = list_first_entry(&ipath->path_list,
					struct inode_path, path_list);
			list_del(&hpath->path_list);
			ipath->link_count--;

			printf("\t\t\t\t\t%s\n", hpath->path);
		}
		if (ipath->link_count) {
			printf(_("Link count anomaly: %d paths left over\n"),
				ipath->link_count);
		}
		free(ipath);
	} while (true);

	/*
	 * Any inodes remaining in the tree at this point indicate inodes whose
	 * paths were not found. This will be free inodes or unlinked but still
	 * open inodes. Either way, a shrink will not succeed until these inodes
	 * are removed from the filesystem.
	 */
	idx = 0;
	do {
		uint64_t	ino;

		ino = get_next_reloc_unlinked(idx);
		if (!ino) {
			if (idx != 0)
				ret = -EBUSY;
			break;
		}
		idx = ino + 1;
		printf(_("No path found for inode 0x%llx!\n"),
				(unsigned long long)ino);
		forget_reloc_ino(ino);
	} while (true);

	return ret;
}

/*
 * Resolve inode numbers to paths via a directory tree walk.
 */
static int
resolve_owner_f(
	int	argc,
	char	**argv)
{
	int	ret;

	if (!is_reloc_populated()) {
		fprintf(stderr,
_("Inode list has not been populated. No inodes to resolve.\n"));
		return 0;
	}

	ret = resolve_target_paths(file->fs_path.fs_dir);
	if (ret) {
		fprintf(stderr,
_("Failed to resolve all paths from mount point %s: %s\n"),
			file->fs_path.fs_dir, strerror(-ret));
		exitcode = 1;
		return 0;
	}

	ret = list_inode_paths();
	if (ret) {
		fprintf(stderr,
_("Failed to list all resolved paths from mount point %s: %s\n"),
			file->fs_path.fs_dir, strerror(-ret));
		exitcode = 1;
		return 0;
	}
	return 0;
}

static void
resolve_owner_help(void)
{
	printf(_(
"\n"
"Resolve inodes owning physical blocks in a given AG.\n"
"This requires the find_owner command to be run first to populate the table\n"
"of inodes that need to have their paths resolved.\n"
"\n"));

}

void
resolve_owner_init(void)
{
	resolve_owner_cmd.name = "resolve_owner";
	resolve_owner_cmd.altname = "rown";
	resolve_owner_cmd.cfunc = resolve_owner_f;
	resolve_owner_cmd.argmin = 0;
	resolve_owner_cmd.argmax = 0;
	resolve_owner_cmd.args = "";
	resolve_owner_cmd.flags = CMD_FLAG_ONESHOT;
	resolve_owner_cmd.oneline = _("Resolve patches to inodes owning physical blocks in a given AG");
	resolve_owner_cmd.help = resolve_owner_help;

	add_command(&resolve_owner_cmd);
}

