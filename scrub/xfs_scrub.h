/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef XFS_SCRUB_XFS_SCRUB_H_
#define XFS_SCRUB_XFS_SCRUB_H_

#define _PATH_PROC_MOUNTS	"/proc/mounts"

extern unsigned int		nr_threads;
extern unsigned int		bg_mode;
extern unsigned int		debug;
extern int			nproc;
extern bool			verbose;
extern long			page_size;

enum scrub_mode {
	SCRUB_MODE_DRY_RUN,
	SCRUB_MODE_PREEN,
	SCRUB_MODE_REPAIR,
};
#define SCRUB_MODE_DEFAULT			SCRUB_MODE_PREEN

enum error_action {
	ERRORS_CONTINUE,
	ERRORS_SHUTDOWN,
};

struct scrub_ctx {
	/* Immutable scrub state. */

	/* Strings we need for presentation */
	char			*mntpoint;
	char			*blkdev;

	/* What does the user want us to do? */
	enum scrub_mode		mode;

	/* How does the user want us to react to errors? */
	enum error_action	error_action;

	/* Mutable scrub state; use lock. */
	pthread_mutex_t		lock;
	unsigned long long	max_errors;
	unsigned long long	runtime_errors;
	unsigned long long	errors_found;
	unsigned long long	warnings_found;
	bool			need_repair;
};

#endif /* XFS_SCRUB_XFS_SCRUB_H_ */
