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

/*
 * Create an xfile of the given size.  The description will be used in the
 * trace output.
 */
int
xfile_create(
	const char		*description,
	struct xfile		**xfilep)
{
	struct xfile		*xf;
	int			error;

	xf = kmem_alloc(sizeof(struct xfile), KM_MAYFAIL);
	if (!xf)
		return -ENOMEM;

	xf->fd = xfile_create_fd(description);
	if (xf->fd < 0) {
		error = -errno;
		kmem_free(xf);
		return error;
	}

	*xfilep = xf;
	return 0;
}

/* Close the file and release all resources. */
void
xfile_destroy(
	struct xfile		*xf)
{
	close(xf->fd);
	kmem_free(xf);
}

static inline loff_t
xfile_maxbytes(
	struct xfile		*xf)
{
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

	ret = pread(xf->fd, buf, count, pos);
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

	ret = pwrite(xf->fd, buf, count, pos);
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

	error = fstat(xf->fd, &ks);
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
	dup2(xf->fd, 0);
	for (i = 3; i < 1024; i++)
		close(i);

	return execvp("od", argv);
}
