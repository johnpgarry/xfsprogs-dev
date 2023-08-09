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
