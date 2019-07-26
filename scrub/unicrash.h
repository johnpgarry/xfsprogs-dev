// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef XFS_SCRUB_UNICRASH_H_
#define XFS_SCRUB_UNICRASH_H_

struct unicrash;

/* Unicode name collision detection. */
#ifdef HAVE_LIBICU

struct dirent;

bool unicrash_dir_init(struct unicrash **ucp, struct scrub_ctx *ctx,
		struct xfs_bstat *bstat);
bool unicrash_xattr_init(struct unicrash **ucp, struct scrub_ctx *ctx,
		struct xfs_bstat *bstat);
bool unicrash_fs_label_init(struct unicrash **ucp, struct scrub_ctx *ctx);
void unicrash_free(struct unicrash *uc);
bool unicrash_check_dir_name(struct unicrash *uc, struct descr *dsc,
		struct dirent *dirent);
bool unicrash_check_xattr_name(struct unicrash *uc, struct descr *dsc,
		const char *attrname);
bool unicrash_check_fs_label(struct unicrash *uc, struct descr *dsc,
		const char *label);
#else
# define unicrash_dir_init(u, c, b)		(true)
# define unicrash_xattr_init(u, c, b)		(true)
# define unicrash_fs_label_init(u, c)		(true)
# define unicrash_free(u)			do {(u) = (u);} while (0)
# define unicrash_check_dir_name(u, d, n)	(true)
# define unicrash_check_xattr_name(u, d, n)	(true)
# define unicrash_check_fs_label(u, d, n)	(true)
#endif /* HAVE_LIBICU */

#endif /* XFS_SCRUB_UNICRASH_H_ */
