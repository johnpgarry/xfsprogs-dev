// SPDX-License-Identifier: GPL-2.0


#include "libxfs_priv.h"

/*
 * Simple memory interface
 */
kmem_zone_t *
kmem_cache_create(const char *name, unsigned int size, unsigned int align,
		  unsigned int slab_flags, void (*ctor)(void *))
{
	kmem_zone_t	*ptr = malloc(sizeof(kmem_zone_t));

	if (ptr == NULL) {
		fprintf(stderr, _("%s: zone init failed (%s, %d bytes): %s\n"),
			progname, name, (int)sizeof(kmem_zone_t),
			strerror(errno));
		exit(1);
	}
	ptr->zone_unitsize = size;
	ptr->zone_name = name;
	ptr->allocated = 0;
	ptr->align = align;
	ptr->ctor = ctor;

	return ptr;
}

int
kmem_zone_destroy(kmem_zone_t *zone)
{
	int	leaked = 0;

	if (getenv("LIBXFS_LEAK_CHECK") && zone->allocated) {
		leaked = 1;
		fprintf(stderr, "zone %s freed with %d items allocated\n",
				zone->zone_name, zone->allocated);
	}
	free(zone);
	return leaked;
}

void *
kmem_cache_alloc(kmem_zone_t *zone, gfp_t flags)
{
	void	*ptr = NULL;

	if (zone->align) {
		int ret;

		ret = posix_memalign(&ptr, zone->align, zone->zone_unitsize);
		if (ret)
			errno = ret;
	} else {
		ptr = malloc(zone->zone_unitsize);
	}

	if (ptr == NULL) {
		fprintf(stderr, _("%s: zone alloc failed (%s, %d bytes): %s\n"),
			progname, zone->zone_name, zone->zone_unitsize,
			strerror(errno));
		exit(1);
	}

	if (zone->ctor)
		zone->ctor(ptr);
	zone->allocated++;
	return ptr;
}

void *
kmem_cache_zalloc(kmem_zone_t *zone, gfp_t flags)
{
	void	*ptr = kmem_cache_alloc(zone, flags);

	memset(ptr, 0, zone->zone_unitsize);
	return ptr;
}

void *
kmem_alloc(size_t size, int flags)
{
	void	*ptr = malloc(size);

	if (ptr == NULL) {
		fprintf(stderr, _("%s: malloc failed (%d bytes): %s\n"),
			progname, (int)size, strerror(errno));
		exit(1);
	}
	return ptr;
}

void *
kvmalloc(size_t size, gfp_t flags)
{
	if (flags & __GFP_ZERO)
		return kmem_zalloc(size, 0);
	return kmem_alloc(size, 0);
}

void *
kmem_zalloc(size_t size, int flags)
{
	void	*ptr = kmem_alloc(size, flags);

	memset(ptr, 0, size);
	return ptr;
}

void *
krealloc(void *ptr, size_t new_size, int flags)
{
	ptr = realloc(ptr, new_size);
	if (ptr == NULL) {
		fprintf(stderr, _("%s: realloc failed (%d bytes): %s\n"),
			progname, (int)new_size, strerror(errno));
		exit(1);
	}
	return ptr;
}
