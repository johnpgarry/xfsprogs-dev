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

/* Dump a histogram to stdout. */
void
hist_print(
	const struct histogram	*hs)
{
	unsigned int		i;

	printf("%7s %7s %7s %7s %6s\n",
		_("from"), _("to"), _("extents"), _("blocks"), _("pct"));
	for (i = 0; i < hs->nr_buckets; i++) {
		if (hs->buckets[i].count == 0)
			continue;

		printf("%7lld %7lld %7lld %7lld %6.2f\n",
				hs->buckets[i].low, hs->buckets[i].high,
				hs->buckets[i].count, hs->buckets[i].blocks,
				hs->buckets[i].blocks * 100.0 / hs->totblocks);
	}
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
