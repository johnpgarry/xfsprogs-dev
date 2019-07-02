// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include "ptvar.h"
#include "counter.h"

/*
 * Per-Thread Counters
 *
 * This is a global counter object that uses per-thread counters to
 * count things without having to content for a single shared lock.
 * Provided we know the number of threads that will be accessing the
 * counter, each thread gets its own thread-specific counter variable.
 * Changing the value is fast, though retrieving the value is expensive
 * and approximate.
 */
struct ptcounter {
	struct ptvar	*var;
};

/* Initialize per-thread counter. */
struct ptcounter *
ptcounter_init(
	size_t			nr)
{
	struct ptcounter	*p;
	int			ret;

	p = malloc(sizeof(struct ptcounter));
	if (!p)
		return NULL;
	ret = ptvar_alloc(nr, sizeof(uint64_t), &p->var);
	if (ret) {
		free(p);
		return NULL;
	}
	return p;
}

/* Free per-thread counter. */
void
ptcounter_free(
	struct ptcounter	*ptc)
{
	ptvar_free(ptc->var);
	free(ptc);
}

/* Add a quantity to the counter. */
void
ptcounter_add(
	struct ptcounter	*ptc,
	int64_t			nr)
{
	uint64_t		*p;
	int			ret;

	p = ptvar_get(ptc->var, &ret);
	assert(ret == 0);
	*p += nr;
}

static int
ptcounter_val_helper(
	struct ptvar		*ptv,
	void			*data,
	void			*foreach_arg)
{
	uint64_t		*sum = foreach_arg;
	uint64_t		*count = data;

	*sum += *count;
	return 0;
}

/* Return the approximate value of this counter. */
uint64_t
ptcounter_value(
	struct ptcounter	*ptc)
{
	uint64_t		sum = 0;

	ptvar_foreach(ptc->var, ptcounter_val_helper, &sum);
	return sum;
}
