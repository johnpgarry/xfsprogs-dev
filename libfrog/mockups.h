// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_MOCKUPS_H__
#define __LIBFROG_MOCKUPS_H__

/* Mockups of kernel data structures. */

typedef struct spinlock {
} spinlock_t;

#define spin_lock_init(lock)	((void) 0)

#define spin_lock(a)		((void) 0)
#define spin_unlock(a)		((void) 0)

#endif /* __LIBFROG_MOCKUPS_H__ */
