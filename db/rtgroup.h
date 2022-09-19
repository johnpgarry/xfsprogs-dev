// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef DB_RTGROUP_H_
#define DB_RTGROUP_H_

extern const struct field	rtsb_flds[];
extern const struct field	rtsb_hfld[];

extern const struct field	rgbitmap_flds[];
extern const struct field	rgbitmap_hfld[];

extern const struct field	rgsummary_flds[];
extern const struct field	rgsummary_hfld[];

extern void	rtsb_init(void);
extern int	rtsb_size(void *obj, int startoff, int idx);

#endif /* DB_RTGROUP_H_ */
