// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2008 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __KMEM_H__
#define __KMEM_H__

void kmem_start_leak_check(void);
bool kmem_found_leaks(void);

#define KM_NOFS		0x0004u
#define KM_MAYFAIL	0x0008u
#define KM_LARGE	0x0010u
#define KM_NOLOCKDEP	0x0020u

struct kmem_cache {
	int		zone_unitsize;	/* Size in bytes of zone unit */
	int		allocated;	/* debug: How many allocated? */
	unsigned int	align;
	const char	*zone_name;	/* tag name */
	void		(*ctor)(void *);
};

typedef unsigned int __bitwise gfp_t;

#define GFP_KERNEL	((__force gfp_t)0)
#define GFP_NOFS	((__force gfp_t)0)
#define __GFP_NOFAIL	((__force gfp_t)0)
#define __GFP_NOLOCKDEP	((__force gfp_t)0)

#define __GFP_ZERO	(__force gfp_t)1

struct kmem_cache * kmem_cache_create(const char *name, unsigned int size,
		unsigned int align, unsigned int slab_flags,
		void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *);

extern void	*kmem_cache_alloc(struct kmem_cache *, gfp_t);
extern void	*kmem_cache_zalloc(struct kmem_cache *, gfp_t);

static inline void
kmem_cache_free(struct kmem_cache *zone, void *ptr)
{
	zone->allocated--;
	free(ptr);
}

extern void	*kmem_alloc(size_t, int);
extern void	*kvmalloc(size_t, gfp_t);
extern void	*kmem_zalloc(size_t, int);

static inline void
kmem_free(const void *ptr) {
	free((void *)ptr);
}

extern void	*krealloc(void *, size_t, int);

#endif
