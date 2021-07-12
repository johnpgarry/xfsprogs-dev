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

struct rb_root {
};

#define RB_ROOT 		(struct rb_root) { }

typedef struct wait_queue_head {
} wait_queue_head_t;

#define init_waitqueue_head(wqh)	do { } while(0)

struct rhashtable {
};

struct rcu_head {
};

#define call_rcu(arg, func)		(func(arg))

struct delayed_work {
};

#define INIT_DELAYED_WORK(work, func)	do { } while(0)
#define cancel_delayed_work_sync(work)	do { } while(0)

#endif /* __LIBFROG_MOCKUPS_H__ */
