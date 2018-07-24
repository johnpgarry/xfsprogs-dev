// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef _XR_VERSIONS_H
#define _XR_VERSIONS_H

#ifndef EXTERN
#define EXTERN extern
#endif /* EXTERN */

/*
 * filesystem feature global vars, set to 1 if the feature
 * is on, 0 otherwise
 */

EXTERN int		fs_attributes;
EXTERN int		fs_attributes2;
EXTERN int		fs_inode_nlink;
EXTERN int		fs_quotas;
EXTERN int		fs_aligned_inodes;
EXTERN int		fs_sb_feature_bits;
EXTERN int		fs_has_extflgbit;

/*
 * inode chunk alignment, fsblocks
 */

EXTERN xfs_extlen_t	fs_ino_alignment;

/*
 * modify superblock to reflect current state of global fs
 * feature vars above
 */
void			update_sb_version(xfs_mount_t *mp);

/*
 * parse current sb to set above feature vars
 */
int			parse_sb_version(xfs_sb_t *sb);

#endif /* _XR_VERSIONS_H */
