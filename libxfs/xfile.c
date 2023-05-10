// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs_priv.h"
#include "libxfs.h"
#include "libxfs/xfile.h"
#include "libfrog/util.h"
#ifdef HAVE_MEMFD_NOEXEC_SEAL
# include <linux/memfd.h>
#endif
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

/*
 * Swappable Temporary Memory
 * ==========================
 *
 * Offline checking sometimes needs to be able to stage a large amount of data
 * in memory.  This information might not fit in the available memory and it
 * doesn't all need to be accessible at all times.  In other words, we want an
 * indexed data buffer to store data that can be paged out.
 *
 * memfd files meet those requirements.  Therefore, the xfile mechanism uses
 * one to store our staging data.  The xfile must be freed with xfile_destroy.
 *
 * xfiles assume that the caller will handle all required concurrency
 * management; file locks are not taken.
 */

/* Figure out the xfile block size here */
unsigned int		XFB_BLOCKSIZE;
unsigned int		XFB_BSHIFT;

void
xfile_libinit(void)
{
	long		ret = sysconf(_SC_PAGESIZE);

	/* If we don't find a power-of-two page size, go with 4k. */
	if (ret < 0 || !is_power_of_2(ret))
		ret = 4096;

	XFB_BLOCKSIZE = ret;
	XFB_BSHIFT = libxfs_highbit32(XFB_BLOCKSIZE);
}

/*
 * Open a memory-backed fd to back an xfile.  We require close-on-exec here,
 * because these memfd files function as windowed RAM and hence should never
 * be shared with other processes.
 */
static int
xfile_create_fd(
	const char		*description)
{
	int			fd = -1;
	int			ret;

#ifdef HAVE_MEMFD_CLOEXEC

# ifdef HAVE_MEMFD_NOEXEC_SEAL
	/*
	 * Starting with Linux 6.3, there's a new MFD_NOEXEC_SEAL flag that
	 * disables the longstanding memfd behavior that files are created with
	 * the executable bit set, and seals the file against it being turned
	 * back on.  Using this bit on older kernels produces EINVAL, so we
	 * try this twice.
	 */
	fd = memfd_create(description, MFD_CLOEXEC | MFD_NOEXEC_SEAL);
	if (fd >= 0)
		goto got_fd;
# endif /* HAVE_MEMFD_NOEXEC_SEAL */

	/* memfd_create exists in kernel 3.17 (2014) and glibc 2.27 (2018). */
	fd = memfd_create(description, MFD_CLOEXEC);
	if (fd >= 0)
		goto got_fd;
#endif /* HAVE_MEMFD_CLOEXEC */

#ifdef HAVE_O_TMPFILE
	/*
	 * O_TMPFILE exists as of kernel 3.11 (2013), which means that if we
	 * find it, we're pretty safe in assuming O_CLOEXEC exists too.
	 */
	fd = open("/dev/shm", O_TMPFILE | O_CLOEXEC | O_RDWR, 0600);
	if (fd >= 0)
		goto got_fd;

	fd = open("/tmp", O_TMPFILE | O_CLOEXEC | O_RDWR, 0600);
	if (fd >= 0)
		goto got_fd;
#endif

#ifdef HAVE_MKOSTEMP_CLOEXEC
	/*
	 * mkostemp exists as of glibc 2.7 (2007) and O_CLOEXEC exists as of
	 * kernel 2.6.23 (2007).
	 */
	fd = mkostemp("libxfsXXXXXX", O_CLOEXEC);
	if (fd >= 0)
		goto got_fd;
#endif

#if !defined(HAVE_MEMFD_CLOEXEC) && \
    !defined(HAVE_O_TMPFILE) && \
    !defined(HAVE_MKOSTEMP_CLOEXEC)
# error System needs memfd_create, O_TMPFILE, or O_CLOEXEC to build!
#endif

	if (!errno)
		errno = EOPNOTSUPP;
	return -1;
got_fd:
	/*
	 * Turn off mode bits we don't want -- group members and others should
	 * not have access to the xfile, nor it be executable.  memfds are
	 * created with mode 0777, but we'll be careful just in case the other
	 * implementations fail to set 0600.
	 */
	ret = fchmod(fd, 0600);
	if (ret)
		perror("disabling xfile executable bit");

	return fd;
}

struct xfile_fcb {
	struct list_head	fcb_list;
	int			fd;
	unsigned int		refcount;
};

static LIST_HEAD(fcb_list);
static pthread_mutex_t fcb_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Create a new memfd. */
static inline int
xfile_fcb_create(
	const char		*description,
	struct xfile_fcb	**fcbp)
{
	struct xfile_fcb	*fcb;
	int			fd;

	fd = xfile_create_fd(description);
	if (fd < 0)
		return -errno;

	fcb = malloc(sizeof(struct xfile_fcb));
	if (!fcb) {
		close(fd);
		return -ENOMEM;
	}

	list_head_init(&fcb->fcb_list);
	fcb->fd = fd;
	fcb->refcount = 1;

	*fcbp = fcb;
	return 0;
}

/* Release an xfile control block */
static void
xfile_fcb_irele(
	struct xfile_fcb	*fcb,
	loff_t			pos,
	uint64_t		len)
{
	/*
	 * If this memfd is linked only to itself, it's private, so we can
	 * close it without taking any locks.
	 */
	if (list_empty(&fcb->fcb_list)) {
		close(fcb->fd);
		free(fcb);
		return;
	}

	pthread_mutex_lock(&fcb_mutex);
	if (--fcb->refcount == 0) {
		/* If we're the last user of this memfd file, kill it fast. */
		list_del(&fcb->fcb_list);
		close(fcb->fd);
		free(fcb);
	} else if (len > 0) {
		struct stat	statbuf;
		int		ret;

		/*
		 * If we were using the end of a partitioned file, free the
		 * address space.  IOWs, bonus points if you delete these in
		 * reverse-order of creation.
		 */
		ret = fstat(fcb->fd, &statbuf);
		if (!ret && statbuf.st_size == pos + len) {
			ret = ftruncate(fcb->fd, pos);
		}
	}
	pthread_mutex_unlock(&fcb_mutex);
}

/*
 * Find an memfd that can accomodate the given amount of address space.
 */
static int
xfile_fcb_find(
	const char		*description,
	uint64_t		maxrange,
	loff_t			*pos,
	struct xfile_fcb	**fcbp)
{
	struct xfile_fcb	*fcb;
	int			ret;
	int			error;

	/* No maximum range means that the caller gets a private memfd. */
	if (maxrange == 0) {
		*pos = 0;
		return xfile_fcb_create(description, fcbp);
	}

	pthread_mutex_lock(&fcb_mutex);

	/*
	 * If we only need a certain number of byte range, look for one with
	 * available file range.
	 */
	list_for_each_entry(fcb, &fcb_list, fcb_list) {
		struct stat	statbuf;

		ret = fstat(fcb->fd, &statbuf);
		if (ret)
			continue;

		ret = ftruncate(fcb->fd, statbuf.st_size + maxrange);
		if (ret)
			continue;

		fcb->refcount++;
		*pos = statbuf.st_size;
		*fcbp = fcb;
		goto out_unlock;
	}

	/* Otherwise, open a new memfd and add it to our list. */
	error = xfile_fcb_create(description, &fcb);
	if (error)
		return error;

	ret = ftruncate(fcb->fd, maxrange);
	if (ret) {
		error = -errno;
		xfile_fcb_irele(fcb, 0, maxrange);
		return error;
	}

	list_add_tail(&fcb->fcb_list, &fcb_list);
	*pos = 0;
	*fcbp = fcb;

out_unlock:
	pthread_mutex_unlock(&fcb_mutex);
	return error;
}

/*
 * Create an xfile of the given size.  The description will be used in the
 * trace output.
 */
int
xfile_create(
	const char		*description,
	unsigned long long	maxrange,
	struct xfile		**xfilep)
{
	struct xfile		*xf;
	int			error;

	xf = kmem_alloc(sizeof(struct xfile), KM_MAYFAIL);
	if (!xf)
		return -ENOMEM;

	error = xfile_fcb_find(description, maxrange, &xf->partition_pos,
			&xf->fcb);
	if (error) {
		kmem_free(xf);
		return error;
	}

	xf->partition_bytes = maxrange;
	*xfilep = xf;
	return 0;
}

/* Close the file and release all resources. */
void
xfile_destroy(
	struct xfile		*xf)
{
	xfile_fcb_irele(xf->fcb, xf->partition_pos, xf->partition_bytes);
	kmem_free(xf);
}

static inline loff_t
xfile_maxbytes(
	struct xfile		*xf)
{
	if (xf->partition_bytes > 0)
		return xf->partition_bytes;

	if (sizeof(loff_t) == 8)
		return LLONG_MAX;
	return LONG_MAX;
}

/*
 * Read a memory object directly from the xfile's page cache.  Unlike regular
 * pread, we return -E2BIG and -EFBIG for reads that are too large or at too
 * high an offset, instead of truncating the read.  Otherwise, we return
 * bytes read or an error code, like regular pread.
 */
ssize_t
xfile_pread(
	struct xfile		*xf,
	void			*buf,
	size_t			count,
	loff_t			pos)
{
	ssize_t			ret;

	if (count > INT_MAX)
		return -E2BIG;
	if (xfile_maxbytes(xf) - pos < count)
		return -EFBIG;

	ret = pread(xf->fcb->fd, buf, count, pos + xf->partition_pos);
	if (ret >= 0)
		return ret;
	return -errno;
}

/*
 * Write a memory object directly to the xfile's page cache.  Unlike regular
 * pwrite, we return -E2BIG and -EFBIG for writes that are too large or at too
 * high an offset, instead of truncating the write.  Otherwise, we return
 * bytes written or an error code, like regular pwrite.
 */
ssize_t
xfile_pwrite(
	struct xfile		*xf,
	const void		*buf,
	size_t			count,
	loff_t			pos)
{
	ssize_t			ret;

	if (count > INT_MAX)
		return -E2BIG;
	if (xfile_maxbytes(xf) - pos < count)
		return -EFBIG;

	ret = pwrite(xf->fcb->fd, buf, count, pos + xf->partition_pos);
	if (ret >= 0)
		return ret;
	return -errno;
}

/* Compute the number of bytes used by a xfile. */
unsigned long long
xfile_bytes(
	struct xfile		*xf)
{
	struct xfile_stat	xs;
	int			ret;

	if (xf->partition_bytes > 0) {
		loff_t		data_pos = xf->partition_pos;
		loff_t		stop_pos = data_pos + xf->partition_bytes;
		loff_t		hole_pos;
		unsigned long long bytes = 0;

		data_pos = lseek(xf->fcb->fd, data_pos, SEEK_DATA);
		while (data_pos >= 0 && data_pos < stop_pos) {
			hole_pos = lseek(xf->fcb->fd, data_pos, SEEK_HOLE);
			if (hole_pos < 0) {
				/* save error, break */
				data_pos = hole_pos;
				break;
			}
			if (hole_pos >= stop_pos) {
				bytes += stop_pos - data_pos;
				return bytes;
			}
			bytes += hole_pos - data_pos;

			data_pos = lseek(xf->fcb->fd, hole_pos, SEEK_DATA);
		}
		if (data_pos < 0) {
			if (errno == ENXIO)
				return bytes;
			return xf->partition_bytes;
		}

		return bytes;
	}

	ret = xfile_stat(xf, &xs);
	if (ret)
		return 0;

	return xs.bytes;
}

/* Query stat information for an xfile. */
int
xfile_stat(
	struct xfile		*xf,
	struct xfile_stat	*statbuf)
{
	struct stat		ks;
	int			error;

	if (xf->partition_bytes > 0) {
		statbuf->size = xf->partition_bytes;
		statbuf->bytes = xf->partition_bytes;
		return 0;
	}

	error = fstat(xf->fcb->fd, &ks);
	if (error)
		return -errno;

	statbuf->size = ks.st_size;
	statbuf->bytes = (unsigned long long)ks.st_blocks << 9;
	return 0;
}

/* Dump an xfile to stdout. */
int
xfile_dump(
	struct xfile		*xf)
{
	char			*argv[] = {"od", "-tx1", "-Ad", "-c", NULL};
	pid_t			child;
	int			i;

	child = fork();
	if (child != 0) {
		int		wstatus;

		wait(&wstatus);
		return wstatus == 0 ? 0 : -EIO;
	}

	/* reroute our xfile to stdin and shut everything else */
	dup2(xf->fcb->fd, 0);
	for (i = 3; i < 1024; i++)
		close(i);

	return execvp("od", argv);
}

/* Ensure that there is storage backing the given range. */
int
xfile_prealloc(
	struct xfile	*xf,
	loff_t		pos,
	uint64_t	count)
{
	int		error;

	count = min(count, xfile_maxbytes(xf) - pos);
	error = fallocate(xf->fcb->fd, 0, pos + xf->partition_pos, count);
	if (error)
		return -errno;
	return 0;
}

/* Discard pages backing a range of the xfile. */
void
xfile_discard(
	struct xfile		*xf,
	loff_t			pos,
	unsigned long long	count)
{
	fallocate(xf->fcb->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			pos, count);
}
