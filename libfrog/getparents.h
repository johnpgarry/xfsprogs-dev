// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef	__LIBFROG_GETPARENTS_H_
#define	__LIBFROG_GETPARENTS_H_

struct path_list;

struct parent_rec {
	uint64_t	p_ino;
	uint32_t	p_gen;
	uint32_t	p_flags;
	unsigned char	p_name[MAXNAMELEN];
};

/* This is the root directory. */
#define PARENT_IS_ROOT	(1U << 0)

typedef int (*walk_parent_fn)(const struct parent_rec *rec, void *arg);
typedef int (*walk_path_fn)(const char *mntpt, const struct path_list *path,
		void *arg);

int fd_walk_parents(int fd, walk_parent_fn fn, void *arg);
int handle_walk_parents(void *hanp, size_t hanlen, walk_parent_fn fn,
		void *arg);

int fd_walk_parent_paths(int fd, walk_path_fn fn, void *arg);
int handle_walk_parent_paths(void *hanp, size_t hanlen, walk_path_fn fn,
		void *arg);

int fd_to_path(int fd, char *path, size_t pathlen);
int handle_to_path(void *hanp, size_t hlen, char *path, size_t pathlen);

#endif /* __LIBFROG_GETPARENTS_H_ */
