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
#ifndef XFS_SCRUB_COMMON_H_
#define XFS_SCRUB_COMMON_H_

/*
 * When reporting a defective metadata object to the console, this
 * is the size of the buffer to use to store the description of that
 * item.
 */
#define DESCR_BUFSZ	256

bool xfs_scrub_excessive_errors(struct scrub_ctx *ctx);

enum error_level {
	S_ERROR	= 0,
	S_WARN,
	S_INFO,
};

void __str_out(struct scrub_ctx *ctx, const char *descr, enum error_level level,
	       int error, const char *file, int line, const char *format, ...);

#define str_errno(ctx, str) \
	__str_out(ctx, str, S_ERROR,	errno,	__FILE__, __LINE__, NULL)
#define str_error(ctx, str, ...) \
	__str_out(ctx, str, S_ERROR,	0,	__FILE__, __LINE__, __VA_ARGS__)
#define str_warn(ctx, str, ...) \
	__str_out(ctx, str, S_WARN,	0,	__FILE__, __LINE__, __VA_ARGS__)
#define str_info(ctx, str, ...) \
	__str_out(ctx, str, S_INFO,	0,	__FILE__, __LINE__, __VA_ARGS__)

#define dbg_printf(fmt, ...) \
	do {if (debug > 1) {printf(fmt, __VA_ARGS__);}} while (0)

/* Is this debug tweak enabled? */
static inline bool
debug_tweak_on(
	const char		*name)
{
	return debug && getenv(name) != NULL;
}

#endif /* XFS_SCRUB_COMMON_H_ */
