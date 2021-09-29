// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011 RedHat, Inc.
 * All Rights Reserved.
 */
#ifndef __ATOMIC_H__
#define __ATOMIC_H__

/*
 * Atomics are provided by liburcu.
 *
 * API and guidelines for which operations provide memory barriers is here:
 *
 * https://github.com/urcu/userspace-rcu/blob/master/doc/uatomic-api.md
 *
 * Unlike the kernel, the same interface supports 32 and 64 bit atomic integers.
 */
#include <urcu/uatomic.h>
#include "spinlock.h"

typedef	int32_t	atomic_t;
typedef	int64_t	atomic64_t;

#define atomic_read(a)		uatomic_read(a)
#define atomic_set(a, v)	uatomic_set(a, v)
#define atomic_add(v, a)	uatomic_add(a, v)
#define atomic_sub(v, a)	uatomic_sub(a, v)
#define atomic_inc(a)		uatomic_inc(a)
#define atomic_dec(a)		uatomic_dec(a)
#define atomic_inc_return(a)	uatomic_add_return(a, 1)
#define atomic_dec_return(a)	uatomic_sub_return(a, 1)
#define atomic_dec_and_test(a)	(atomic_dec_return(a) == 0)
#define cmpxchg(a, o, n)	uatomic_cmpxchg(a, o, n);

static inline bool atomic_add_unless(atomic_t *a, int v, int u)
{
	int r = atomic_read(a);
	int n, o;

	do {
		o = r;
		if (o == u)
			break;
		n = o + v;
		r = uatomic_cmpxchg(a, o, n);
	} while (r != o);

	return o != u;
}

static inline bool atomic_dec_and_lock(atomic_t *a, spinlock_t *lock)
{
	if (atomic_add_unless(a, -1, 1))
		return 0;

	spin_lock(lock);
	if (atomic_dec_and_test(a))
		return 1;
	spin_unlock(lock);
	return 0;
}

#define atomic64_read(a)	uatomic_read(a)
#define atomic64_set(a, v)	uatomic_set(a, v)
#define atomic64_add(v, a)	uatomic_add(a, v)
#define atomic64_sub(v, a)	uatomic_sub(a, v)
#define atomic64_inc(a)		uatomic_inc(a)
#define atomic64_dec(a)		uatomic_dec(a)

#endif /* __ATOMIC_H__ */
