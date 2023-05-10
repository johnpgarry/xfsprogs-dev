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
 * xfblob structure.
 */
struct strblobs {
	struct xfblob		*strings;
};

/* Initialize a string blob structure. */
int
strblobs_init(
	const char		*descr,
	struct strblobs		**sblobs)
{
	struct strblobs		*sb;
	int			error;

	sb = malloc(sizeof(struct strblobs));
	if (!sb)
		return ENOMEM;

	error = -xfblob_create(descr, &sb->strings);
	if (error)
		goto out_free;

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

	xfblob_destroy(sb->strings);
	free(sb);
	*sblobs = NULL;
}

/* Store a string and return a cookie for its retrieval. */
int
strblobs_store(
	struct strblobs		*sblobs,
	xfblob_cookie		*str_cookie,
	const unsigned char	*str,
	unsigned int		str_len)
{
	return -xfblob_store(sblobs->strings, str_cookie, str, str_len);
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
