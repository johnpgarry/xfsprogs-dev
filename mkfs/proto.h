// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2004-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef MKFS_PROTO_H_
#define	MKFS_PROTO_H_

extern char *setup_proto (char *fname);
extern void parse_proto (xfs_mount_t *mp, struct fsxattr *fsx, char **pp);
extern void res_failed (int err);

#endif /* MKFS_PROTO_H_ */
