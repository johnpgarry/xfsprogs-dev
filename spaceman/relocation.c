// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libfrog/fsgeom.h"
#ifdef USE_RADIX_TREE_FOR_INUMS
#include "libfrog/radix-tree.h"
#else
#include "libfrog/avl64.h"
#endif /* USE_RADIX_TREE_FOR_INUMS */
#include "libfrog/paths.h"
#include "command.h"
#include "init.h"
#include "space.h"
#include "input.h"
#include "relocation.h"
#include "handle.h"

static unsigned long long inode_count;
static unsigned long long inode_paths;

unsigned long long
get_reloc_count(void)
{
	return inode_count;
}

#ifdef USE_RADIX_TREE_FOR_INUMS
static RADIX_TREE(relocation_data, 0);

bool
is_reloc_populated(void)
{
	return relocation_data.rnode != NULL;
}

bool
test_reloc_iflag(
	uint64_t	ino,
	unsigned int	flag)
{
	return radix_tree_tag_get(&relocation_data, ino, flag);
}

void
set_reloc_iflag(
	uint64_t	ino,
	unsigned int	flag)
{
	if (!radix_tree_lookup(&relocation_data, ino)) {
		radix_tree_insert(&relocation_data, ino, UNLINKED_IPATH);
		if (flag != INODE_PATH)
			inode_count++;
	}
	if (flag == INODE_PATH)
		inode_paths++;

	radix_tree_tag_set(&relocation_data, ino, flag);
}

struct inode_path *
get_next_reloc_ipath(
	uint64_t	ino)
{
	struct inode_path	*ipath;
	int			ret;

	ret = radix_tree_gang_lookup_tag(&relocation_data, (void **)&ipath,
			ino, 1, INODE_PATH);
	if (!ret)
		return NULL;
	return ipath;
}

uint64_t
get_next_reloc_unlinked(
	uint64_t	ino)
{
	uint64_t	next_ino;
	int		ret;

	ret = radix_tree_gang_lookup(&relocation_data, (void **)&next_ino, ino,
			1);
	if (!ret)
		return 0;
	return next_ino;
}

/*
 * Return a pointer to a pointer where the caller can read or write a pointer
 * to an inode path structure.
 *
 * The pointed-to pointer will be set to UNLINKED_IPATH if there is no ipath
 * associated with this inode but the inode has been flagged for relocation.
 *
 * Returns NULL if the inode is not flagged for relocation.
 */
struct inode_path **
get_reloc_ipath_slot(
	uint64_t		ino)
{
	struct inode_path	**slot;

	slot = (struct inode_path **)radix_tree_lookup_slot(&relocation_data,
			ino);
	if (!slot || *slot == NULL)
		return NULL;
	return slot;
}

void
forget_reloc_ino(
	uint64_t		ino)
{
	radix_tree_delete(&relocation_data, ino);
}
#else
struct reloc_node {
	struct avl64node	node;
	uint64_t		ino;
	struct inode_path	*ipath;
	unsigned int		flags;
};

static uint64_t
reloc_start(
	struct avl64node	*node)
{
	struct reloc_node	*rln;

	rln = container_of(node, struct reloc_node, node);
	return rln->ino;
}

static uint64_t
reloc_end(
	struct avl64node	*node)
{
	struct reloc_node	*rln;

	rln = container_of(node, struct reloc_node, node);
	return rln->ino + 1;
}

static struct avl64ops reloc_ops = {
	reloc_start,
	reloc_end,
};

static struct avl64tree_desc	relocation_data = {
	.avl_ops = &reloc_ops,
};

bool
is_reloc_populated(void)
{
	return relocation_data.avl_firstino != NULL;
}

static inline struct reloc_node *
reloc_lookup(
	uint64_t		ino)
{
	avl64node_t		*node;

	node = avl64_find(&relocation_data, ino);
	if (!node)
		return NULL;

	return container_of(node, struct reloc_node, node);
}

static inline struct reloc_node *
reloc_insert(
	uint64_t		ino)
{
	struct reloc_node	*rln;
	avl64node_t		*node;

	rln = malloc(sizeof(struct reloc_node));
	if (!rln)
		return NULL;

	rln->node.avl_nextino = NULL;
	rln->ino = ino;
	rln->ipath = UNLINKED_IPATH;
	rln->flags = 0;

	node = avl64_insert(&relocation_data, &rln->node);
	if (node == NULL) {
		free(rln);
		return NULL;
	}

	return rln;
}

bool
test_reloc_iflag(
	uint64_t		ino,
	unsigned int		flag)
{
	struct reloc_node	*rln;

	rln = reloc_lookup(ino);
	if (!rln)
		return false;

	return rln->flags & flag;
}

void
set_reloc_iflag(
	uint64_t		ino,
	unsigned int		flag)
{
	struct reloc_node	*rln;

	rln = reloc_lookup(ino);
	if (!rln) {
		rln = reloc_insert(ino);
		if (!rln)
			abort();
		if (flag != INODE_PATH)
			inode_count++;
	}
	if (flag == INODE_PATH)
		inode_paths++;

	rln->flags |= flag;
}

#define avl_for_each_range_safe(pos, n, l, first, last) \
	for (pos = (first), n = pos->avl_nextino, l = (last)->avl_nextino; \
			pos != (l); \
			pos = n, n = pos ? pos->avl_nextino : NULL)

struct inode_path *
get_next_reloc_ipath(
	uint64_t		ino)
{
	struct avl64node	*firstn;
	struct avl64node	*lastn;
	struct avl64node	*pos;
	struct avl64node	*n;
	struct avl64node	*l;
	struct reloc_node	*rln;

	avl64_findranges(&relocation_data, ino - 1, -1ULL, &firstn, &lastn);
	if (firstn == NULL && lastn == NULL)
		return NULL;

	avl_for_each_range_safe(pos, n, l, firstn, lastn) {
		rln = container_of(pos, struct reloc_node, node);

		if (rln->flags & INODE_PATH)
			return rln->ipath;
	}

	return NULL;
}

uint64_t
get_next_reloc_unlinked(
	uint64_t		ino)
{
	struct avl64node	*firstn;
	struct avl64node	*lastn;
	struct avl64node	*pos;
	struct avl64node	*n;
	struct avl64node	*l;
	struct reloc_node	*rln;

	avl64_findranges(&relocation_data, ino - 1, -1ULL, &firstn, &lastn);
	if (firstn == NULL && lastn == NULL)
		return 0;

	avl_for_each_range_safe(pos, n, l, firstn, lastn) {
		rln = container_of(pos, struct reloc_node, node);

		if (!(rln->flags & INODE_PATH))
			return rln->ino;
	}

	return 0;
}

struct inode_path **
get_reloc_ipath_slot(
	uint64_t		ino)
{
	struct reloc_node	*rln;

	rln = reloc_lookup(ino);
	if (!rln)
		return NULL;

	return &rln->ipath;
}

void
forget_reloc_ino(
	uint64_t		ino)
{
	struct reloc_node	*rln;

	rln = reloc_lookup(ino);
	if (!rln)
		return;

	avl64_delete(&relocation_data, &rln->node);
	free(rln);
}
#endif /* USE_RADIX_TREE_FOR_INUMS */

static struct cmdinfo relocate_cmd;

static int
relocate_targets_to_ag(
	const char		*mnt,
	xfs_agnumber_t		dst_agno)
{
	struct inode_path	*ipath;
	uint64_t		idx = 0;
	int			ret = 0;

	do {
		struct xfs_fd	xfd = {0};
		struct stat	st;

		/* lookup first relocation target */
		ipath = get_next_reloc_ipath(idx);
		if (!ipath)
			break;

		/* XXX: don't handle hard link cases yet */
		if (ipath->link_count > 1) {
			fprintf(stderr,
		"FIXME! Skipping hardlinked inode at path %s\n",
				ipath->path);
			goto next;
		}


		ret = stat(ipath->path, &st);
		if (ret) {
			fprintf(stderr, _("stat(%s) failed: %s\n"),
				ipath->path, strerror(errno));
			goto next;
		}

		if (!S_ISREG(st.st_mode)) {
			fprintf(stderr,
		_("FIXME! Skipping %s: not a regular file.\n"),
				ipath->path);
			goto next;
		}

		ret = xfd_open(&xfd, ipath->path, O_RDONLY);
		if (ret) {
			fprintf(stderr, _("xfd_open(%s) failed: %s\n"),
				ipath->path, strerror(-ret));
			goto next;
		}

		/* move to destination AG */
		ret = relocate_file_to_ag(mnt, ipath->path, &xfd, dst_agno);
		xfd_close(&xfd);

		/*
		 * If the destination AG has run out of space, we do not remove
		 * this inode from relocation data so it will be immediately
		 * retried in the next AG. Other errors will be fatal.
		 */
		if (ret < 0)
			return ret;
next:
		/* remove from relocation data */
		idx = ipath->ino + 1;
		forget_reloc_ino(ipath->ino);
	} while (ret != -ENOSPC);

	return ret;
}

static int
relocate_targets(
	const char		*mnt,
	xfs_agnumber_t		highest_agno)
{
	xfs_agnumber_t		dst_agno = 0;
	int			ret;

	for (dst_agno = 0; dst_agno <= highest_agno; dst_agno++) {
		ret = relocate_targets_to_ag(mnt, dst_agno);
		if (ret == -ENOSPC)
			continue;
		break;
	}
	return ret;
}

/*
 * Relocate all the user objects in an AG to lower numbered AGs.
 */
static int
relocate_f(
	int		argc,
	char		**argv)
{
	xfs_agnumber_t	target_agno = -1;
	xfs_agnumber_t	highest_agno = -1;
	xfs_agnumber_t	log_agno;
	void		*fshandle;
	size_t		fshdlen;
	int		c;
	int		ret;

	while ((c = getopt(argc, argv, "a:h:")) != EOF) {
		switch (c) {
		case 'a':
			target_agno = cvt_u32(optarg, 10);
			if (errno) {
				fprintf(stderr, _("bad target agno value %s\n"),
					optarg);
				return command_usage(&relocate_cmd);
			}
			break;
		case 'h':
			highest_agno = cvt_u32(optarg, 10);
			if (errno) {
				fprintf(stderr, _("bad highest agno value %s\n"),
					optarg);
				return command_usage(&relocate_cmd);
			}
			break;
		default:
			return command_usage(&relocate_cmd);
		}
	}

	if (optind != argc)
		return command_usage(&relocate_cmd);

	if (target_agno == -1) {
		fprintf(stderr, _("Target AG must be specified!\n"));
		return command_usage(&relocate_cmd);
	}

	log_agno = cvt_fsb_to_agno(&file->xfd, file->xfd.fsgeom.logstart);
	if (target_agno <= log_agno) {
		fprintf(stderr,
_("Target AG %d must be higher than the journal AG (AG %d). Aborting.\n"),
			target_agno, log_agno);
		goto out_fail;
	}

	if (target_agno >= file->xfd.fsgeom.agcount) {
		fprintf(stderr,
_("Target AG %d does not exist. Filesystem only has %d AGs\n"),
			target_agno, file->xfd.fsgeom.agcount);
		goto out_fail;
	}

	if (highest_agno == -1)
		highest_agno = target_agno - 1;

	if (highest_agno >= target_agno) {
		fprintf(stderr,
_("Highest destination AG %d must be less than target AG %d. Aborting.\n"),
			highest_agno, target_agno);
		goto out_fail;
	}

	if (is_reloc_populated()) {
		fprintf(stderr,
_("Relocation data populated from previous commands. Aborting.\n"));
		goto out_fail;
	}

	/* this is so we can use fd_to_handle() later on */
	ret = path_to_fshandle(file->fs_path.fs_dir, &fshandle, &fshdlen);
	if (ret < 0) {
		fprintf(stderr, _("Cannot get fshandle for mount %s: %s\n"),
			file->fs_path.fs_dir, strerror(errno));
		goto out_fail;
	}

	ret = find_relocation_targets(target_agno);
	if (ret) {
		fprintf(stderr,
_("Failure during target discovery. Aborting.\n"));
		goto out_fail;
	}

	ret = resolve_target_paths(file->fs_path.fs_dir);
	if (ret) {
		fprintf(stderr,
_("Failed to resolve all paths from mount point %s: %s\n"),
			file->fs_path.fs_dir, strerror(-ret));
		goto out_fail;
	}

	ret = relocate_targets(file->fs_path.fs_dir, highest_agno);
	if (ret) {
		fprintf(stderr,
_("Failed to relocate all targets out of AG %d: %s\n"),
			target_agno, strerror(-ret));
		goto out_fail;
	}

	return 0;
out_fail:
	exitcode = 1;
	return 0;
}

static void
relocate_help(void)
{
	printf(_(
"\n"
"Relocate all the user data and metadata in an AG.\n"
"\n"
"This function will discover all the relocatable objects in a single AG and\n"
"move them to a lower AG as preparation for a shrink operation.\n"
"\n"
"	-a <agno>	Allocation group to empty\n"
"	-h <agno>	Highest target AG allowed to relocate into\n"
"\n"));

}

void
relocate_init(void)
{
	relocate_cmd.name = "relocate";
	relocate_cmd.altname = "relocate";
	relocate_cmd.cfunc = relocate_f;
	relocate_cmd.argmin = 2;
	relocate_cmd.argmax = 4;
	relocate_cmd.args = "-a agno [-h agno]";
	relocate_cmd.flags = CMD_FLAG_ONESHOT;
	relocate_cmd.oneline = _("Relocate data in an AG.");
	relocate_cmd.help = relocate_help;

	add_command(&relocate_cmd);
}
