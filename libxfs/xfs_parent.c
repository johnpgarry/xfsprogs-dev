// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2024 Oracle.
 * All rights reserved.
 */
#include "libxfs_priv.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_trace.h"
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_da_format.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_attr_sf.h"
#include "xfs_bmap.h"
#include "xfs_parent.h"
#include "xfs_da_format.h"
#include "xfs_format.h"
#include "xfs_trans_space.h"

/*
 * Parent pointer attribute handling.
 *
 * Because the attribute value is a filename component, it will never be longer
 * than 255 bytes. This means the attribute will always be a local format
 * attribute as it is xfs_attr_leaf_entsize_local_max() for v5 filesystems will
 * always be larger than this (max is 75% of block size).
 *
 * Creating a new parent attribute will always create a new attribute - there
 * should never, ever be an existing attribute in the tree for a new inode.
 * ENOSPC behavior is problematic - creating the inode without the parent
 * pointer is effectively a corruption, so we allow parent attribute creation
 * to dip into the reserve block pool to avoid unexpected ENOSPC errors from
 * occurring.
 */

/* Return true if parent pointer EA name is valid. */
bool
xfs_parent_namecheck(
	struct xfs_mount			*mp,
	const struct xfs_parent_name_rec	*rec,
	size_t					reclen,
	unsigned int				attr_flags)
{
	if (!(attr_flags & XFS_ATTR_PARENT))
		return false;

	/* pptr updates use logged xattrs, so we should never see this flag */
	if (attr_flags & XFS_ATTR_INCOMPLETE)
		return false;

	if (reclen != sizeof(struct xfs_parent_name_rec))
		return false;

	/* Only one namespace bit allowed. */
	if (hweight32(attr_flags & XFS_ATTR_NSP_ONDISK_MASK) > 1)
		return false;

	return true;
}

/* Return true if parent pointer EA value is valid. */
bool
xfs_parent_valuecheck(
	struct xfs_mount		*mp,
	const void			*value,
	size_t				valuelen)
{
	if (valuelen == 0 || valuelen > XFS_PARENT_DIRENT_NAME_MAX_SIZE)
		return false;

	if (value == NULL)
		return false;

	return true;
}

/* Return true if the ondisk parent pointer is consistent. */
bool
xfs_parent_hashcheck(
	struct xfs_mount		*mp,
	const struct xfs_parent_name_rec *rec,
	const void			*value,
	size_t				valuelen)
{
	struct xfs_name			dname = {
		.name			= value,
		.len			= valuelen,
	};
	xfs_ino_t			p_ino;

	/* Valid dirent name? */
	if (!xfs_dir2_namecheck(value, valuelen))
		return false;

	/* Valid inode number? */
	p_ino = be64_to_cpu(rec->p_ino);
	if (!xfs_verify_dir_ino(mp, p_ino))
		return false;

	/* Namehash matches name? */
	return be32_to_cpu(rec->p_namehash) == xfs_dir2_hashname(mp, &dname);
}
