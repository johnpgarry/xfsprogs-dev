// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_COUNTER_H_
#define XFS_SCRUB_COUNTER_H_

struct ptcounter;
struct ptcounter *ptcounter_init(size_t nr);
void ptcounter_free(struct ptcounter *ptc);
void ptcounter_add(struct ptcounter *ptc, int64_t nr);
uint64_t ptcounter_value(struct ptcounter *ptc);

#endif /* XFS_SCRUB_COUNTER_H_ */
