// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_FSCOUNTERS_H_
#define XFS_SCRUB_FSCOUNTERS_H_

int scrub_scan_estimate_blocks(struct scrub_ctx *ctx,
		unsigned long long *d_blocks, unsigned long long *d_bfree,
		unsigned long long *r_blocks, unsigned long long *r_bfree,
		unsigned long long *f_files_used);
int scrub_count_all_inodes(struct scrub_ctx *ctx, uint64_t *count);

#endif /* XFS_SCRUB_FSCOUNTERS_H_ */
