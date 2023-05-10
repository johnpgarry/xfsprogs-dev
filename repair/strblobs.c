// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "libxfs/xfile.h"
#include "libxfs/xfblob.h"
#include "repair/strblobs.h"

/*
 * String Blob Structure
 * =====================
 *
 * This data structure wraps the storage of strings with explicit length in an
 * xfblob structure.  It stores a hashtable of string checksums to provide
 * fast(ish) lookups of existing strings to enable deduplication of the strings
 * contained within.
 */
struct strblob_hashent {
	struct strblob_hashent	*next;

	xfblob_cookie		str_cookie;
	unsigned int		str_len;
	xfs_dahash_t		str_hash;
};

struct strblobs {
	struct xfblob		*strings;
	unsigned int		nr_buckets;

	struct strblob_hashent	*buckets[];
};

static inline size_t strblobs_sizeof(unsigned int nr_buckets)
{
	return sizeof(struct strblobs) +
			(nr_buckets * sizeof(struct strblobs_hashent *));
}

/* Initialize a string blob structure. */
int
strblobs_init(
	const char		*descr,
	unsigned int		hash_buckets,
	struct strblobs		**sblobs)
{
	struct strblobs		*sb;
	int			error;

	sb = calloc(strblobs_sizeof(hash_buckets), 1);
	if (!sb)
		return ENOMEM;

	error = -xfblob_create(descr, &sb->strings);
	if (error)
		goto out_free;

	sb->nr_buckets = hash_buckets;
	*sblobs = sb;
	return 0;

out_free:
	free(sb);
	return error;
}

/* Deconstruct a string blob structure. */
void
strblobs_destroy(
	struct strblobs		**sblobs)
{
	struct strblobs		*sb = *sblobs;
	struct strblob_hashent	*ent, *ent_next;
	unsigned int		bucket;

	for (bucket = 0; bucket < sb->nr_buckets; bucket++) {
		ent = sb->buckets[bucket];
		while (ent != NULL) {
			ent_next = ent->next;
			free(ent);
			ent = ent_next;
		}
	}

	xfblob_destroy(sb->strings);
	free(sb);
	*sblobs = NULL;
}

/*
 * Search the string hashtable for a matching entry.  Sets sets the cookie and
 * returns 0 if one is found; ENOENT if there is no match; or a positive errno.
 */
static int
__strblobs_lookup(
	struct strblobs		*sblobs,
	xfblob_cookie		*str_cookie,
	const unsigned char	*str,
	unsigned int		str_len,
	xfs_dahash_t		str_hash)
{
	struct strblob_hashent	*ent;
	char			*buf = NULL;
	unsigned int		bucket;
	int			error;

	bucket = str_hash % sblobs->nr_buckets;
	ent = sblobs->buckets[bucket];

	for (ent = sblobs->buckets[bucket]; ent != NULL; ent = ent->next) {
		if (ent->str_len != str_len || ent->str_hash != str_hash)
			continue;

		if (!buf) {
			buf = malloc(str_len);
			if (!buf)
				return ENOMEM;
		}

		error = strblobs_load(sblobs, ent->str_cookie, buf, str_len);
		if (error)
			goto out;

		if (memcmp(str, buf, str_len))
			continue;

		*str_cookie = ent->str_cookie;
		goto out;
	}
	error = ENOENT;

out:
	free(buf);
	return error;
}

/*
 * Search the string hashtable for a matching entry.  Sets sets the cookie and
 * returns 0 if one is found; ENOENT if there is no match; or a positive errno.
 */
int
strblobs_lookup(
	struct strblobs		*sblobs,
	xfblob_cookie		*str_cookie,
	const unsigned char	*str,
	unsigned int		str_len,
	xfs_dahash_t		str_hash)
{
	return __strblobs_lookup(sblobs, str_cookie, str, str_len, str_hash);
}

/* Remember a string in the hashtable. */
static int
strblobs_hash(
	struct strblobs		*sblobs,
	xfblob_cookie		str_cookie,
	const unsigned char	*str,
	unsigned int		str_len,
	xfs_dahash_t		str_hash)
{
	struct strblob_hashent	*ent;
	unsigned int		bucket;

	bucket = str_hash % sblobs->nr_buckets;

	ent = malloc(sizeof(struct strblob_hashent));
	if (!ent)
		return ENOMEM;

	ent->str_cookie = str_cookie;
	ent->str_len = str_len;
	ent->str_hash = str_hash;
	ent->next = sblobs->buckets[bucket];

	sblobs->buckets[bucket] = ent;
	return 0;
}

/* Store a string and return a cookie for its retrieval. */
int
strblobs_store(
	struct strblobs		*sblobs,
	xfblob_cookie		*str_cookie,
	const unsigned char	*str,
	unsigned int		str_len,
	xfs_dahash_t		str_hash)
{
	int			error;

	error = __strblobs_lookup(sblobs, str_cookie, str, str_len, str_hash);
	if (error != ENOENT)
		return error;

	error = -xfblob_store(sblobs->strings, str_cookie, str, str_len);
	if (error)
		return error;

	return strblobs_hash(sblobs, *str_cookie, str, str_len, str_hash);
}

/* Retrieve a previously stored string. */
int
strblobs_load(
	struct strblobs		*sblobs,
	xfblob_cookie		str_cookie,
	unsigned char		*str,
	unsigned int		str_len)
{
	return -xfblob_load(sblobs->strings, str_cookie, str, str_len);
}
