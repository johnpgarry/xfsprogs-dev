// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2012 Red Hat, Inc.
 * Copyright (c) 2017-2024 Oracle.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_HISTOGRAM_H__
#define __LIBFROG_HISTOGRAM_H__

struct histent
{
	/* Low and high size of this bucket */
	long long	low;
	long long	high;

	/* Count of observations recorded */
	long long	count;

	/* Sum of blocks recorded */
	long long	blocks;
};

struct histogram {
	/* Sum of all blocks recorded */
	long long	totblocks;

	/* Count of all observations recorded */
	long long	totexts;

	struct histent	*buckets;

	/* Number of buckets */
	unsigned int	nr_buckets;
};

int hist_add_bucket(struct histogram *hs, long long bucket_low);
void hist_add(struct histogram *hs, long long len);
void hist_init(struct histogram *hs);
void hist_prepare(struct histogram *hs, long long maxlen);
void hist_free(struct histogram *hs);
void hist_print(const struct histogram *hs);
void hist_summarize(const struct histogram *hs);

static inline unsigned int hist_buckets(const struct histogram *hs)
{
	return hs->nr_buckets;
}

#endif /* __LIBFROG_HISTOGRAM_H__ */
