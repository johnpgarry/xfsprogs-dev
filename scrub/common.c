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
#include <stdio.h>
#include <pthread.h>
#include <stdbool.h>
#include "platform_defs.h"
#include "xfs.h"
#include "xfs_scrub.h"
#include "common.h"

/*
 * Reporting Status to the Console
 *
 * We aim for a roughly standard reporting format -- the severity of the
 * status being reported, a textual description of the object being
 * reported, and whatever the status happens to be.
 *
 * Errors are the most severe and reflect filesystem corruption.
 * Warnings indicate that something is amiss and needs the attention of
 * the administrator, but does not constitute a corruption.  Information
 * is merely advisory.
 */

/* Too many errors? Bail out. */
bool
xfs_scrub_excessive_errors(
	struct scrub_ctx	*ctx)
{
	bool			ret;

	pthread_mutex_lock(&ctx->lock);
	ret = ctx->max_errors > 0 && ctx->errors_found >= ctx->max_errors;
	pthread_mutex_unlock(&ctx->lock);

	return ret;
}

static const char *err_str[] = {
	[S_ERROR]	= "Error",
	[S_WARN]	= "Warning",
	[S_INFO]	= "Info",
};

/* Print a warning string and some warning text. */
void
__str_out(
	struct scrub_ctx	*ctx,
	const char		*descr,
	enum error_level	level,
	int			error,
	const char		*file,
	int			line,
	const char		*format,
	...)
{
	FILE			*stream = stderr;
	va_list			args;
	char			buf[DESCR_BUFSZ];

	/* print strerror or format of choice but not both */
	assert(!(error && format));

	if (level >= S_INFO)
		stream = stdout;

	pthread_mutex_lock(&ctx->lock);
	fprintf(stream, "%s: %s: ", _(err_str[level]), descr);
	if (error) {
		fprintf(stream, _("%s."), strerror_r(error, buf, DESCR_BUFSZ));
	} else {
		va_start(args, format);
		vfprintf(stream, format, args);
		va_end(args);
	}

	if (debug)
		fprintf(stream, _(" (%s line %d)"), file, line);
	fprintf(stream, "\n");
	if (stream == stdout)
		fflush(stream);

	if (error)      /* A syscall failed */
		ctx->runtime_errors++;
	else if (level == S_ERROR)
		ctx->errors_found++;
	else if (level == S_WARN)
		ctx->warnings_found++;

	pthread_mutex_unlock(&ctx->lock);
}

double
timeval_subtract(
	struct timeval		*tv1,
	struct timeval		*tv2)
{
	return ((tv1->tv_sec - tv2->tv_sec) +
		((float) (tv1->tv_usec - tv2->tv_usec)) / 1000000);
}

/* Produce human readable disk space output. */
double
auto_space_units(
	unsigned long long	bytes,
	char			**units)
{
	if (debug > 1)
		goto no_prefix;
	if (bytes > (1ULL << 40)) {
		*units = "TiB";
		return (double)bytes / (1ULL << 40);
	} else if (bytes > (1ULL << 30)) {
		*units = "GiB";
		return (double)bytes / (1ULL << 30);
	} else if (bytes > (1ULL << 20)) {
		*units = "MiB";
		return (double)bytes / (1ULL << 20);
	} else if (bytes > (1ULL << 10)) {
		*units = "KiB";
		return (double)bytes / (1ULL << 10);
	}

no_prefix:
	*units = "B";
	return bytes;
}

/* Produce human readable discrete number output. */
double
auto_units(
	unsigned long long	number,
	char			**units)
{
	if (debug > 1)
		goto no_prefix;
	if (number > 1000000000000ULL) {
		*units = "T";
		return number / 1000000000000.0;
	} else if (number > 1000000000ULL) {
		*units = "G";
		return number / 1000000000.0;
	} else if (number > 1000000ULL) {
		*units = "M";
		return number / 1000000.0;
	} else if (number > 1000ULL) {
		*units = "K";
		return number / 1000.0;
	}

no_prefix:
	*units = "";
	return number;
}

/* How many threads to kick off? */
unsigned int
scrub_nproc(
	struct scrub_ctx	*ctx)
{
	if (nr_threads)
		return nr_threads;
	return ctx->nr_io_threads;
}

/*
 * How many threads to kick off for a workqueue?  If we only want one
 * thread, save ourselves the overhead and just run it in the main thread.
 */
unsigned int
scrub_nproc_workqueue(
	struct scrub_ctx	*ctx)
{
	unsigned int		x;

	x = scrub_nproc(ctx);
	if (x == 1)
		x = 0;
	return x;
}
