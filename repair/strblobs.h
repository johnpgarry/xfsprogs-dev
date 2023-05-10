/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __REPAIR_STRBLOBS_H__
#define __REPAIR_STRBLOBS_H__

struct strblobs;

int strblobs_init(const char *descr, unsigned int hash_buckets,
		struct strblobs **sblobs);
void strblobs_destroy(struct strblobs **sblobs);

int strblobs_store(struct strblobs *sblobs, xfblob_cookie *str_cookie,
		const unsigned char *str, unsigned int str_len,
		xfs_dahash_t hash);
int strblobs_load(struct strblobs *sblobs, xfblob_cookie str_cookie,
		unsigned char *str, unsigned int str_len);
int strblobs_lookup(struct strblobs *sblobs, xfblob_cookie *str_cookie,
		const unsigned char *str, unsigned int str_len,
		xfs_dahash_t hash);

#endif /* __REPAIR_STRBLOBS_H__ */
