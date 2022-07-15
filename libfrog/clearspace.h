/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBFROG_CLEARSPACE_H__
#define __LIBFROG_CLEARSPACE_H__

struct clearspace_req;

struct clearspace_init {
	/* Open file and its pathname */
	struct xfs_fd		*xfd;
	const char		*fname;

	/* Which device do we want? */
	bool			is_realtime;
	dev_t			dev;

	/* Range of device to clear. */
	unsigned long long	start;
	unsigned long long	length;

	unsigned int		trace_mask;
};

int clearspace_init(struct clearspace_req **reqp,
		const struct clearspace_init *init);
int clearspace_free(struct clearspace_req **reqp);

int clearspace_run(struct clearspace_req *req);

int clearspace_efficacy(struct clearspace_req *req,
		unsigned long long *cleared_bytes);

/* Debugging levels */

#define CSP_TRACE_FREEZE	(1U << 0)
#define CSP_TRACE_GRAB		(1U << 1)
#define CSP_TRACE_FSMAP		(1U << 2)
#define CSP_TRACE_FSREFS	(1U << 3)
#define CSP_TRACE_BMAPX		(1U << 4)
#define CSP_TRACE_PREP		(1U << 5)
#define CSP_TRACE_TARGET	(1U << 6)
#define CSP_TRACE_DEDUPE	(1U << 7)
#define CSP_TRACE_FALLOC	(1U << 8)
#define CSP_TRACE_EXCHANGE	(1U << 9)
#define CSP_TRACE_XREBUILD	(1U << 10)
#define CSP_TRACE_EFFICACY	(1U << 11)
#define CSP_TRACE_SETUP		(1U << 12)
#define CSP_TRACE_STATUS	(1U << 13)
#define CSP_TRACE_DUMPFILE	(1U << 14)
#define CSP_TRACE_BITMAP	(1U << 15)

#define CSP_TRACE_ALL		(CSP_TRACE_FREEZE | \
				 CSP_TRACE_GRAB | \
				 CSP_TRACE_FSMAP | \
				 CSP_TRACE_FSREFS | \
				 CSP_TRACE_BMAPX | \
				 CSP_TRACE_PREP	 | \
				 CSP_TRACE_TARGET | \
				 CSP_TRACE_DEDUPE | \
				 CSP_TRACE_FALLOC | \
				 CSP_TRACE_EXCHANGE | \
				 CSP_TRACE_XREBUILD | \
				 CSP_TRACE_EFFICACY | \
				 CSP_TRACE_SETUP | \
				 CSP_TRACE_STATUS | \
				 CSP_TRACE_DUMPFILE | \
				 CSP_TRACE_BITMAP)

#endif /* __LIBFROG_CLEARSPACE_H__ */
