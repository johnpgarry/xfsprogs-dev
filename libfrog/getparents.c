// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2017-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "xfs.h"
#include "xfs_arch.h"
#include "list.h"
#include "libfrog/paths.h"
#include "handle.h"
#include "libfrog/getparents.h"

/* Allocate a buffer large enough for some parent pointer records. */
static inline struct xfs_getparents *
alloc_pptr_buf(
	size_t			bufsize)
{
	struct xfs_getparents	*pi;

	pi = calloc(bufsize, 1);
	if (!pi)
		return NULL;
	pi->gp_bufsize = bufsize;
	return pi;
}

/*
 * Walk all parents of the given file handle.  Returns 0 on success or positive
 * errno.
 */
static int
call_getparents(
	int			fd,
	struct xfs_handle	*handle,
	walk_parent_fn		fn,
	void			*arg)
{
	struct xfs_getparents	*pi;
	struct xfs_getparents_rec	*p;
	unsigned int		i;
	ssize_t			ret = -1;

	pi = alloc_pptr_buf(XFS_XATTR_LIST_MAX);
	if (!pi)
		return errno;

	if (handle) {
		memcpy(&pi->gp_handle, handle, sizeof(struct xfs_handle));
		pi->gp_flags = XFS_GETPARENTS_IFLAG_HANDLE;
	}

	ret = ioctl(fd, XFS_IOC_GETPARENTS, pi);
	while (!ret) {
		if (pi->gp_flags & XFS_GETPARENTS_OFLAG_ROOT) {
			struct parent_rec	rec = {
				.p_flags	= PARENT_IS_ROOT,
			};

			ret = fn(&rec, arg);
			goto out_pi;
		}

		for (i = 0; i < pi->gp_count; i++) {
			struct parent_rec	rec = { 0 };

			p = xfs_getparents_rec(pi, i);
			rec.p_ino = p->gpr_ino;
			rec.p_gen = p->gpr_gen;
			strncpy((char *)rec.p_name, (char *)p->gpr_name,
					MAXNAMELEN - 1);

			ret = fn(&rec, arg);
			if (ret)
				goto out_pi;
		}

		if (pi->gp_flags & XFS_GETPARENTS_OFLAG_DONE)
			break;

		ret = ioctl(fd, XFS_IOC_GETPARENTS, pi);
	}
	if (ret)
		ret = errno;

out_pi:
	free(pi);
	return ret;
}

/* Walk all parent pointers of this handle.  Returns 0 or positive errno. */
int
handle_walk_parents(
	void			*hanp,
	size_t			hlen,
	walk_parent_fn		fn,
	void			*arg)
{
	char			*mntpt;
	int			fd;

	if (hlen != sizeof(struct xfs_handle))
		return EINVAL;

	fd = handle_to_fsfd(hanp, &mntpt);
	if (fd < 0)
		return errno;

	return call_getparents(fd, hanp, fn, arg);
}

/* Walk all parent pointers of this fd.  Returns 0 or positive errno. */
int
fd_walk_parents(
	int			fd,
	walk_parent_fn		fn,
	void			*arg)
{
	return call_getparents(fd, NULL, fn, arg);
}

struct walk_ppaths_info {
	walk_path_fn			fn;
	void				*arg;
	char				*mntpt;
	struct path_list		*path;
	int				fd;
};

struct walk_ppath_level_info {
	struct xfs_handle		newhandle;
	struct path_component		*pc;
	struct walk_ppaths_info		*wpi;
};

static int handle_walk_ppath(struct walk_ppaths_info *wpi,
		struct xfs_handle *handle);

static int
handle_walk_ppath_rec(
	const struct parent_rec		*rec,
	void				*arg)
{
	struct walk_ppath_level_info	*wpli = arg;
	struct walk_ppaths_info		*wpi = wpli->wpi;
	int				ret = 0;

	if (rec->p_flags & PARENT_IS_ROOT)
		return wpi->fn(wpi->mntpt, wpi->path, wpi->arg);

	if (path_will_loop(wpi->path, rec->p_ino))
		return 0;

	ret = path_component_change(wpli->pc, rec->p_name,
				strlen((char *)rec->p_name), rec->p_ino);
	if (ret)
		return ret;

	wpli->newhandle.ha_fid.fid_ino = rec->p_ino;
	wpli->newhandle.ha_fid.fid_gen = rec->p_gen;

	path_list_add_parent_component(wpi->path, wpli->pc);
	ret = handle_walk_ppath(wpi, &wpli->newhandle);
	path_list_del_component(wpi->path, wpli->pc);

	return ret;
}

/*
 * Recursively walk all parents of the given file handle; if we hit the
 * fs root then we call the associated function with the constructed path.
 * Returns 0 for success or positive errno.
 */
static int
handle_walk_ppath(
	struct walk_ppaths_info		*wpi,
	struct xfs_handle		*handle)
{
	struct walk_ppath_level_info	*wpli;
	int				ret;

	wpli = malloc(sizeof(struct walk_ppath_level_info));
	if (!wpli)
		return errno;
	wpli->pc = path_component_init("", 0);
	if (!wpli->pc) {
		ret = errno;
		free(wpli);
		return ret;
	}
	wpli->wpi = wpi;
	memcpy(&wpli->newhandle, handle, sizeof(struct xfs_handle));

	ret = call_getparents(wpi->fd, handle, handle_walk_ppath_rec, wpli);

	path_component_free(wpli->pc);
	free(wpli);
	return ret;
}

/*
 * Call the given function on all known paths from the vfs root to the inode
 * described in the handle.  Returns 0 for success or positive errno.
 */
int
handle_walk_parent_paths(
	void			*hanp,
	size_t			hlen,
	walk_path_fn		fn,
	void			*arg)
{
	struct walk_ppaths_info	wpi;
	ssize_t			ret;

	if (hlen != sizeof(struct xfs_handle))
		return EINVAL;

	wpi.fd = handle_to_fsfd(hanp, &wpi.mntpt);
	if (wpi.fd < 0)
		return errno;
	wpi.path = path_list_init();
	if (!wpi.path)
		return errno;
	wpi.fn = fn;
	wpi.arg = arg;

	ret = handle_walk_ppath(&wpi, hanp);
	path_list_free(wpi.path);

	return ret;
}

/*
 * Call the given function on all known paths from the vfs root to the inode
 * referred to by the file description.  Returns 0 or positive errno.
 */
int
fd_walk_parent_paths(
	int			fd,
	walk_path_fn		fn,
	void			*arg)
{
	struct walk_ppaths_info	wpi;
	void			*hanp;
	size_t			hlen;
	int			fsfd;
	int			ret;

	ret = fd_to_handle(fd, &hanp, &hlen);
	if (ret)
		return errno;

	fsfd = handle_to_fsfd(hanp, &wpi.mntpt);
	if (fsfd < 0)
		return errno;
	wpi.fd = fd;
	wpi.path = path_list_init();
	if (!wpi.path)
		return errno;
	wpi.fn = fn;
	wpi.arg = arg;

	ret = handle_walk_ppath(&wpi, hanp);
	path_list_free(wpi.path);

	return ret;
}

struct path_walk_info {
	char			*buf;
	size_t			len;
	size_t			written;
};

/* Helper that stringifies the first full path that we find. */
static int
handle_to_path_walk(
	const char		*mntpt,
	const struct path_list	*path,
	void			*arg)
{
	struct path_walk_info	*pwi = arg;
	int			mntpt_len = strlen(mntpt);
	int			ret;

	/* Trim trailing slashes from the mountpoint */
	while (mntpt_len > 0 && mntpt[mntpt_len - 1] == '/')
		mntpt_len--;

	ret = snprintf(pwi->buf, pwi->len, "%.*s", mntpt_len, mntpt);
	if (ret != mntpt_len)
		return ENAMETOOLONG;
	pwi->written += ret;

	ret = path_list_to_string(path, pwi->buf + ret, pwi->len - ret);
	if (ret < 0)
		return ENAMETOOLONG;

	pwi->written += ret;
	return ECANCELED;
}

/*
 * Return any eligible path to this file handle.  Returns 0 for success or
 * positive errno.
 */
int
handle_to_path(
	void			*hanp,
	size_t			hlen,
	char			*path,
	size_t			pathlen)
{
	struct path_walk_info	pwi = { .buf = path, .len = pathlen };
	int			ret;

	ret = handle_walk_parent_paths(hanp, hlen, handle_to_path_walk, &pwi);
	if (ret && ret != ECANCELED)
		return ret;
	if (!pwi.written)
		return ENODATA;

	path[pwi.written] = 0;
	return 0;
}

/*
 * Return any eligible path to this file description.  Returns 0 for success
 * or positive errno.
 */
int
fd_to_path(
	int			fd,
	char			*path,
	size_t			pathlen)
{
	struct path_walk_info	pwi = { .buf = path, .len = pathlen };
	int			ret;

	ret = fd_walk_parent_paths(fd, handle_to_path_walk, &pwi);
	if (ret && ret != ECANCELED)
		return ret;
	if (!pwi.written)
		return ENODATA;

	path[pwi.written] = 0;
	return ret;
}
