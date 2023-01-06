// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2012 Red Hat, Inc.
 * Copyright (c) 2017-2024 Oracle.
 * All Rights Reserved.
 */
#include "xfs.h"
#include <stdlib.h>
#include <string.h>
#include "platform_defs.h"
#include "libfrog/histogram.h"

/* Create a new bucket with the given low value. */
int
hist_add_bucket(
	struct histogram	*hs,
	long long		bucket_low)
{
	struct histent		*buckets;

	if (hs->nr_buckets == INT_MAX)
		return EFBIG;

	buckets = realloc(hs->buckets,
			(hs->nr_buckets + 1) * sizeof(struct histent));
	if (!buckets)
		return errno;

	hs->buckets = buckets;
	hs->buckets[hs->nr_buckets].low = bucket_low;
	hs->buckets[hs->nr_buckets].count = buckets[hs->nr_buckets].blocks = 0;
	hs->nr_buckets++;
	return 0;
}

/* Add an observation to the histogram. */
void
hist_add(
	struct histogram	*hs,
	long long		len)
{
	unsigned int		i;

	hs->totexts++;
	hs->totblocks += len;
	for (i = 0; i < hs->nr_buckets; i++) {
		if (hs->buckets[i].high >= len) {
			hs->buckets[i].count++;
			hs->buckets[i].blocks += len;
			break;
		}
	}
}

static int
histent_cmp(
	const void		*a,
	const void		*b)
{
	const struct histent	*ha = a;
	const struct histent	*hb = b;

	if (ha->low < hb->low)
		return -1;
	if (ha->low > hb->low)
		return 1;
	return 0;
}

/* Prepare a histogram for bucket configuration. */
void
hist_init(
	struct histogram	*hs)
{
	memset(hs, 0, sizeof(struct histogram));
}

/* Prepare a histogram to receive data observations. */
void
hist_prepare(
	struct histogram	*hs,
	long long		maxlen)
{
	unsigned int		i;

	qsort(hs->buckets, hs->nr_buckets, sizeof(struct histent), histent_cmp);

	for (i = 0; i < hs->nr_buckets; i++) {
		if (i < hs->nr_buckets - 1)
			hs->buckets[i].high = hs->buckets[i + 1].low - 1;
		else
			hs->buckets[i].high = maxlen;
	}
}

/* Free all data associated with a histogram. */
void
hist_free(
	struct histogram	*hs)
{
	free(hs->buckets);
	memset(hs, 0, sizeof(struct histogram));
}

/*
 * Compute the CDF of the free space in decreasing order of extent length.
 * This enables users to determine how much free space is not in the long tail
 * of small extents, e.g. 98% of the free space extents are larger than 31
 * blocks.
 */
int
hist_cdf(
	const struct histogram	*hs,
	struct histogram	*cdf)
{
	struct histent		*buckets;
	int			i = hs->nr_buckets - 1;

	ASSERT(cdf->nr_buckets == 0);
	ASSERT(hs->nr_buckets < INT_MAX);

	if (hs->nr_buckets == 0)
		return 0;

	buckets = calloc(hs->nr_buckets, sizeof(struct histent));
	if (!buckets)
		return errno;

	memset(cdf, 0, sizeof(struct histogram));
	cdf->buckets = buckets;

	cdf->buckets[i].count = hs->buckets[i].count;
	cdf->buckets[i].blocks = hs->buckets[i].blocks;
	i--;

	while (i >= 0) {
		cdf->buckets[i].count = hs->buckets[i].count +
				       cdf->buckets[i + 1].count;

		cdf->buckets[i].blocks = hs->buckets[i].blocks +
					cdf->buckets[i + 1].blocks;
		i--;
	}

	return 0;
}

/* Dump a histogram to stdout. */
void
hist_print(
	const struct histogram	*hs)
{
	struct histogram	cdf = { };
	unsigned int		from_w, to_w, extents_w, blocks_w;
	unsigned int		i;
	int			error;

	error = hist_cdf(hs, &cdf);
	if (error) {
		printf(_("histogram cdf: %s\n"), strerror(error));
		return;
	}

	from_w = to_w = extents_w = blocks_w = 7;
	for (i = 0; i < hs->nr_buckets; i++) {
		char buf[256];

		if (!hs->buckets[i].count)
			continue;

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].low);
		from_w = max(from_w, strlen(buf));

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].high);
		to_w = max(to_w, strlen(buf));

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].count);
		extents_w = max(extents_w, strlen(buf));

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].blocks);
		blocks_w = max(blocks_w, strlen(buf));
	}

	printf("%*s %*s %*s %*s %6s %6s %6s\n",
		from_w, _("from"), to_w, _("to"), extents_w, _("extents"),
		blocks_w, _("blocks"), _("pct"), _("blkcdf"), _("extcdf"));
	for (i = 0; i < hs->nr_buckets; i++) {
		if (hs->buckets[i].count == 0)
			continue;

		printf("%*lld %*lld %*lld %*lld %6.2f %6.2f %6.2f\n",
				from_w, hs->buckets[i].low,
				to_w, hs->buckets[i].high,
				extents_w, hs->buckets[i].count,
				blocks_w, hs->buckets[i].blocks,
				hs->buckets[i].blocks * 100.0 / hs->totblocks,
				cdf.buckets[i].blocks * 100.0 / hs->totblocks,
				cdf.buckets[i].count * 100.0 / hs->totexts);
	}

	hist_free(&cdf);
}

/* Summarize the contents of the histogram. */
void
hist_summarize(
	const struct histogram	*hs)
{
	printf(_("total free extents %lld\n"), hs->totexts);
	printf(_("total free blocks %lld\n"), hs->totblocks);
	printf(_("average free extent size %g\n"),
			(double)hs->totblocks / (double)hs->totexts);
}

/* Copy the contents of src to dest. */
void
hist_import(
	struct histogram	*dest,
	const struct histogram	*src)
{
	unsigned int		i;

	ASSERT(dest->nr_buckets == src->nr_buckets);

	dest->totblocks += src->totblocks;
	dest->totexts += src->totexts;

	for (i = 0; i < dest->nr_buckets; i++) {
		ASSERT(dest->buckets[i].low == src->buckets[i].low);
		ASSERT(dest->buckets[i].high == src->buckets[i].high);

		dest->buckets[i].count += src->buckets[i].count;
		dest->buckets[i].blocks += src->buckets[i].blocks;
	}
}

/*
 * Move the contents of src to dest and reinitialize src.  dst must not
 * contain any observations or buckets.
 */
void
hist_move(
	struct histogram	*dest,
	struct histogram	*src)
{
	ASSERT(dest->nr_buckets == 0);
	ASSERT(dest->totexts == 0);

	memcpy(dest, src, sizeof(struct histogram));
	hist_init(src);
}
