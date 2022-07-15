/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020-2024 Oracle.  All rights reserved.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_FILE_EXCHANGE_H__
#define __LIBFROG_FILE_EXCHANGE_H__

void xfrog_file_exchange_require_file2_fresh(struct xfs_exch_range *req,
		struct xfs_bulkstat *bulkstat);
int xfrog_file_exchange_prep(struct xfs_fd *file2, uint64_t flags,
		int64_t file2_offset, int file1_fd, int64_t file1_offset,
		int64_t length, struct xfs_exch_range *req);
int xfrog_file_exchange(struct xfs_fd *xfd, struct xfs_exch_range *req);

#endif	/* __LIBFROG_FILE_EXCHANGE_H__ */
