// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/radix-tree.h"
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
