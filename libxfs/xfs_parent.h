// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2024 Oracle.
 * All Rights Reserved.
 */
#ifndef	__XFS_PARENT_H__
#define	__XFS_PARENT_H__

/* Metadata validators */
bool xfs_parent_namecheck(struct xfs_mount *mp,
		const struct xfs_parent_name_rec *rec, size_t reclen,
		unsigned int attr_flags);
bool xfs_parent_valuecheck(struct xfs_mount *mp, const void *value,
		size_t valuelen);
bool xfs_parent_hashcheck(struct xfs_mount *mp,
		const struct xfs_parent_name_rec *rec, const void *value,
		size_t valuelen);

#endif /* __XFS_PARENT_H__ */
