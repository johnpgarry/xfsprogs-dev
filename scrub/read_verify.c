// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include "ptvar.h"
#include "workqueue.h"
#include "path.h"
#include "xfs_scrub.h"
#include "common.h"
#include "counter.h"
#include "disk.h"
#include "read_verify.h"
#include "progress.h"

/*
 * Read Verify Pool
 *
 * Manages the data block read verification phase.  The caller schedules
 * verification requests, which are then scheduled to be run by a thread
 * pool worker.  Adjacent (or nearly adjacent) requests can be combined
 * to reduce overhead when free space fragmentation is high.  The thread
 * pool takes care of issuing multiple IOs to the device, if possible.
 */

/*
 * Perform all IO in 32M chunks.  This cannot exceed 65536 sectors
 * because that's the biggest SCSI VERIFY(16) we dare to send.
 */
#define RVP_IO_MAX_SIZE		(33554432)
#define RVP_IO_MAX_SECTORS	(RVP_IO_MAX_SIZE >> BBSHIFT)

/* Tolerate 64k holes in adjacent read verify requests. */
#define RVP_IO_BATCH_LOCALITY	(65536)

struct read_verify {
	void			*io_end_arg;
	struct disk		*io_disk;
	uint64_t		io_start;	/* bytes */
	uint64_t		io_length;	/* bytes */
};

struct read_verify_pool {
	struct workqueue	wq;		/* thread pool */
	struct scrub_ctx	*ctx;		/* scrub context */
	void			*readbuf;	/* read buffer */
	struct ptcounter	*verified_bytes;
	struct ptvar		*rvstate;	/* combines read requests */
	struct disk		*disk;		/* which disk? */
	read_verify_ioerr_fn_t	ioerr_fn;	/* io error callback */
	size_t			miniosz;	/* minimum io size, bytes */
	int			errors_seen;
};

/*
 * Create a thread pool to run read verifiers.
 *
 * @disk is the disk we want to verify.
 * @miniosz is the minimum size of an IO to expect (in bytes).
 * @ioerr_fn will be called when IO errors occur.
 * @submitter_threads is the number of threads that may be sending verify
 * requests at any given time.
 */
struct read_verify_pool *
read_verify_pool_init(
	struct scrub_ctx		*ctx,
	struct disk			*disk,
	size_t				miniosz,
	read_verify_ioerr_fn_t		ioerr_fn,
	unsigned int			submitter_threads)
{
	struct read_verify_pool		*rvp;
	bool				ret;
	int				error;

	rvp = calloc(1, sizeof(struct read_verify_pool));
	if (!rvp)
		return NULL;

	error = posix_memalign((void **)&rvp->readbuf, page_size,
			RVP_IO_MAX_SIZE);
	if (error || !rvp->readbuf)
		goto out_free;
	error = ptcounter_alloc(nproc, &rvp->verified_bytes);
	if (error)
		goto out_buf;
	rvp->miniosz = miniosz;
	rvp->ctx = ctx;
	rvp->disk = disk;
	rvp->ioerr_fn = ioerr_fn;
	rvp->errors_seen = false;
	error = ptvar_alloc(submitter_threads, sizeof(struct read_verify),
			&rvp->rvstate);
	if (error)
		goto out_counter;
	/* Run in the main thread if we only want one thread. */
	if (nproc == 1)
		nproc = 0;
	ret = workqueue_create(&rvp->wq, (struct xfs_mount *)rvp,
			disk_heads(disk));
	if (ret)
		goto out_rvstate;
	return rvp;

out_rvstate:
	ptvar_free(rvp->rvstate);
out_counter:
	ptcounter_free(rvp->verified_bytes);
out_buf:
	free(rvp->readbuf);
out_free:
	free(rvp);
	return NULL;
}

/* Abort all verification work. */
void
read_verify_pool_abort(
	struct read_verify_pool		*rvp)
{
	if (!rvp->errors_seen)
		rvp->errors_seen = ECANCELED;
	workqueue_terminate(&rvp->wq);
}

/* Finish up any read verification work. */
void
read_verify_pool_flush(
	struct read_verify_pool		*rvp)
{
	workqueue_terminate(&rvp->wq);
}

/* Finish up any read verification work and tear it down. */
void
read_verify_pool_destroy(
	struct read_verify_pool		*rvp)
{
	workqueue_destroy(&rvp->wq);
	ptvar_free(rvp->rvstate);
	ptcounter_free(rvp->verified_bytes);
	free(rvp->readbuf);
	free(rvp);
}

/*
 * Issue a read-verify IO in big batches.
 */
static void
read_verify(
	struct workqueue		*wq,
	xfs_agnumber_t			agno,
	void				*arg)
{
	struct read_verify		*rv = arg;
	struct read_verify_pool		*rvp;
	unsigned long long		verified = 0;
	ssize_t				sz;
	ssize_t				len;
	int				ret;

	rvp = (struct read_verify_pool *)wq->wq_ctx;
	while (rv->io_length > 0) {
		len = min(rv->io_length, RVP_IO_MAX_SIZE);
		dbg_printf("diskverify %d %"PRIu64" %zu\n", rvp->disk->d_fd,
				rv->io_start, len);
		sz = disk_read_verify(rvp->disk, rvp->readbuf, rv->io_start,
				len);
		if (sz < 0) {
			dbg_printf("IOERR %d %"PRIu64" %zu\n",
					rvp->disk->d_fd, rv->io_start, len);
			/* IO error, so try the next logical block. */
			len = rvp->miniosz;
			rvp->ioerr_fn(rvp->ctx, rvp->disk, rv->io_start, len,
					errno, rv->io_end_arg);
		}

		progress_add(len);
		verified += len;
		rv->io_start += len;
		rv->io_length -= len;
	}

	free(rv);
	ret = ptcounter_add(rvp->verified_bytes, verified);
	if (ret) {
		str_liberror(rvp->ctx, ret,
				_("updating bytes verified counter"));
		rvp->errors_seen = true;
	}
}

/* Queue a read verify request. */
static bool
read_verify_queue(
	struct read_verify_pool		*rvp,
	struct read_verify		*rv)
{
	struct read_verify		*tmp;
	bool				ret;

	dbg_printf("verify fd %d start %"PRIu64" len %"PRIu64"\n",
			rvp->disk->d_fd, rv->io_start, rv->io_length);

	/* Worker thread saw a runtime error, don't queue more. */
	if (rvp->errors_seen)
		return false;

	/* Otherwise clone the request and queue the copy. */
	tmp = malloc(sizeof(struct read_verify));
	if (!tmp) {
		str_errno(rvp->ctx, _("allocating read-verify request"));
		rvp->errors_seen = true;
		return false;
	}

	memcpy(tmp, rv, sizeof(*tmp));

	ret = workqueue_add(&rvp->wq, read_verify, 0, tmp);
	if (ret) {
		str_liberror(rvp->ctx, ret, _("queueing read-verify work"));
		free(tmp);
		rvp->errors_seen = true;
		return false;
	}
	rv->io_length = 0;
	return true;
}

/*
 * Issue an IO request.  We'll batch subsequent requests if they're
 * within 64k of each other
 */
bool
read_verify_schedule_io(
	struct read_verify_pool		*rvp,
	uint64_t			start,
	uint64_t			length,
	void				*end_arg)
{
	struct read_verify		*rv;
	uint64_t			req_end;
	uint64_t			rv_end;
	int				ret;

	assert(rvp->readbuf);
	rv = ptvar_get(rvp->rvstate, &ret);
	if (ret)
		return false;
	req_end = start + length;
	rv_end = rv->io_start + rv->io_length;

	/*
	 * If we have a stashed IO, we haven't changed fds, the error
	 * reporting is the same, and the two extents are close,
	 * we can combine them.
	 */
	if (rv->io_length > 0 &&
	    end_arg == rv->io_end_arg &&
	    ((start >= rv->io_start && start <= rv_end + RVP_IO_BATCH_LOCALITY) ||
	     (rv->io_start >= start &&
	      rv->io_start <= req_end + RVP_IO_BATCH_LOCALITY))) {
		rv->io_start = min(rv->io_start, start);
		rv->io_length = max(req_end, rv_end) - rv->io_start;
	} else  {
		/* Otherwise, issue the stashed IO (if there is one) */
		if (rv->io_length > 0)
			return read_verify_queue(rvp, rv);

		/* Stash the new IO. */
		rv->io_start = start;
		rv->io_length = length;
		rv->io_end_arg = end_arg;
	}

	return true;
}

/* Force any stashed IOs into the verifier. */
bool
read_verify_force_io(
	struct read_verify_pool		*rvp)
{
	struct read_verify		*rv;
	bool				moveon;
	int				ret;

	assert(rvp->readbuf);
	rv = ptvar_get(rvp->rvstate, &ret);
	if (ret)
		return false;
	if (rv->io_length == 0)
		return true;

	moveon = read_verify_queue(rvp, rv);
	if (moveon)
		rv->io_length = 0;
	return moveon;
}

/* How many bytes has this process verified? */
uint64_t
read_verify_bytes(
	struct read_verify_pool		*rvp)
{
	uint64_t			ret;

	ptcounter_value(rvp->verified_bytes, &ret);
	return ret;
}
