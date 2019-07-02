// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef LIBFROG_PERCPU_H_
#define LIBFROG_PERCPU_H_

struct ptvar;

typedef int (*ptvar_iter_fn)(struct ptvar *ptv, void *data, void *foreach_arg);

int ptvar_alloc(size_t nr, size_t size, struct ptvar **pptv);
void ptvar_free(struct ptvar *ptv);
void *ptvar_get(struct ptvar *ptv, int *ret);
int ptvar_foreach(struct ptvar *ptv, ptvar_iter_fn fn, void *foreach_arg);

#endif /* LIBFROG_PERCPU_H_ */
