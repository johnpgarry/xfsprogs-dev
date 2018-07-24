// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef LIBFROG_PERCPU_H_
#define LIBFROG_PERCPU_H_

struct ptvar;

typedef bool (*ptvar_iter_fn)(struct ptvar *ptv, void *data, void *foreach_arg);

struct ptvar *ptvar_init(size_t nr, size_t size);
void ptvar_free(struct ptvar *ptv);
void *ptvar_get(struct ptvar *ptv);
bool ptvar_foreach(struct ptvar *ptv, ptvar_iter_fn fn, void *foreach_arg);

#endif /* LIBFROG_PERCPU_H_ */
