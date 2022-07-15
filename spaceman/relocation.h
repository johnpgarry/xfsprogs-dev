// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Red Hat, Inc.
 * All Rights Reserved.
 */
#ifndef XFS_SPACEMAN_RELOCATION_H_
#define XFS_SPACEMAN_RELOCATION_H_

bool is_reloc_populated(void);
unsigned long long get_reloc_count(void);

/*
 * Tags for the relocation_data tree that indicate what it contains and the
 * discovery information that needed to be stored.
 */
#define MOVE_INODE	0
#define MOVE_BLOCKS	1
#define INODE_PATH	2

bool test_reloc_iflag(uint64_t ino, unsigned int flag);
void set_reloc_iflag(uint64_t ino, unsigned int flag);
struct inode_path *get_next_reloc_ipath(uint64_t ino);
uint64_t get_next_reloc_unlinked(uint64_t ino);
struct inode_path **get_reloc_ipath_slot(uint64_t ino);
void forget_reloc_ino(uint64_t ino);

/*
 * When the entry in the relocation_data tree is tagged with INODE_PATH, the
 * entry contains a structure that tracks the discovered paths to the inode. If
 * the inode has multiple hard links, then we chain each individual path found
 * via the path_list and record the number of paths in the link_count entry.
 */
struct inode_path {
	uint64_t		ino;
	struct list_head	path_list;
	uint32_t		link_count;
	char			path[1];
};

/*
 * Sentinel value for inodes that we have to move but haven't yet found a path
 * to.
 */
#define UNLINKED_IPATH		((struct inode_path *)1)

struct inode_path *ipath_alloc(const char *path, const struct stat *st);

int find_relocation_targets(xfs_agnumber_t agno);
int relocate_file_to_ag(const char *mnt, struct inode_path *ipath,
			struct xfs_fd *xfd, xfs_agnumber_t agno);
int resolve_target_paths(const char *mntpt);

#endif /* XFS_SPACEMAN_RELOCATION_H_ */
