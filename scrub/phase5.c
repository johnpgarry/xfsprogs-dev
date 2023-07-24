// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#ifdef HAVE_LIBATTR
# include <attr/attributes.h>
#endif
#include <linux/fs.h>
#include "handle.h"
#include "list.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "libfrog/fsgeom.h"
#include "libfrog/scrub.h"
#include "libfrog/bitmap.h"
#include "libfrog/bulkstat.h"
#include "xfs_scrub.h"
#include "common.h"
#include "inodes.h"
#include "progress.h"
#include "scrub.h"
#include "descr.h"
#include "unicrash.h"
#include "repair.h"

/* Phase 5: Full inode scans and check directory connectivity. */

struct ncheck_state {
	struct scrub_ctx	*ctx;

	/* Have we aborted this scan? */
	bool			aborted;

	/* Is this the last time we're going to process deferred inodes? */
	bool			last_call;

	/* Did we fix at least one thing while walking @cur->deferred? */
	bool			fixed_something;

	/* Lock for this structure */
	pthread_mutex_t		lock;

	/*
	 * Inodes that are involved with directory tree structure corruptions
	 * are marked here.  This will be NULL until the first corruption is
	 * noted.
	 */
	struct bitmap		*new_deferred;

	/*
	 * Inodes that we're reprocessing due to earlier directory tree
	 * structure corruption problems are marked here.  This will be NULL
	 * during the first (parallel) inode scan.
	 */
	struct bitmap		*cur_deferred;
};

/*
 * Warn about problematic bytes in a directory/attribute name.  That means
 * terminal control characters and escape sequences, since that could be used
 * to do something naughty to the user's computer and/or break scripts.  XFS
 * doesn't consider any byte sequence invalid, so don't flag these as errors.
 *
 * Returns 0 for success or -1 for error.  This function logs errors.
 */
static int
simple_check_name(
	struct scrub_ctx	*ctx,
	struct descr		*dsc,
	const char		*namedescr,
	const char		*name)
{
	const char		*p;
	bool			bad = false;
	char			*errname;

	/* Complain about zero length names. */
	if (*name == '\0' && should_warn_about_name(ctx)) {
		str_warn(ctx, descr_render(dsc), _("Zero length name found."));
		return 0;
	}

	/* control characters */
	for (p = name; *p; p++) {
		if ((*p >= 1 && *p <= 31) || *p == 127) {
			bad = true;
			break;
		}
	}

	if (bad && should_warn_about_name(ctx)) {
		errname = string_escape(name);
		if (!errname) {
			str_errno(ctx, descr_render(dsc));
			return -1;
		}
		str_info(ctx, descr_render(dsc),
_("Control character found in %s name \"%s\"."),
				namedescr, errname);
		free(errname);
	}

	return 0;
}

/*
 * Iterate a directory looking for filenames with problematic
 * characters.
 */
static int
check_dirent_names(
	struct scrub_ctx	*ctx,
	struct descr		*dsc,
	int			*fd,
	struct xfs_bulkstat	*bstat)
{
	struct unicrash		*uc = NULL;
	DIR			*dir;
	struct dirent		*dentry;
	int			ret;

	dir = fdopendir(*fd);
	if (!dir) {
		str_errno(ctx, descr_render(dsc));
		return errno;
	}
	*fd = -1; /* closedir will close *fd for us */

	ret = unicrash_dir_init(&uc, ctx, bstat);
	if (ret) {
		str_liberror(ctx, ret, descr_render(dsc));
		goto out_unicrash;
	}

	errno = 0;
	dentry = readdir(dir);
	while (dentry) {
		if (uc)
			ret = unicrash_check_dir_name(uc, dsc, dentry);
		else
			ret = simple_check_name(ctx, dsc, _("directory"),
					dentry->d_name);
		if (ret) {
			str_liberror(ctx, ret, descr_render(dsc));
			break;
		}
		errno = 0;
		dentry = readdir(dir);
	}
	if (errno) {
		ret = errno;
		str_liberror(ctx, ret, descr_render(dsc));
	}
	unicrash_free(uc);

out_unicrash:
	closedir(dir);
	return ret;
}

#ifdef HAVE_LIBATTR
/* Routines to scan all of an inode's xattrs for name problems. */
struct attrns_decode {
	int			flags;
	const char		*name;
};

static const struct attrns_decode attr_ns[] = {
	{0,			"user"},
	{ATTR_ROOT,		"system"},
	{ATTR_SECURE,		"secure"},
	{0, NULL},
};

/*
 * Check all the xattr names in a particular namespace of a file handle
 * for Unicode normalization problems or collisions.
 */
static int
check_xattr_ns_names(
	struct scrub_ctx		*ctx,
	struct descr			*dsc,
	struct xfs_handle		*handle,
	struct xfs_bulkstat		*bstat,
	const struct attrns_decode	*attr_ns)
{
	struct attrlist_cursor		cur;
	char				attrbuf[XFS_XATTR_LIST_MAX];
	char				keybuf[XATTR_NAME_MAX + 1];
	struct attrlist			*attrlist = (struct attrlist *)attrbuf;
	struct attrlist_ent		*ent;
	struct unicrash			*uc = NULL;
	int				i;
	int				error;

	error = unicrash_xattr_init(&uc, ctx, bstat);
	if (error) {
		str_liberror(ctx, error, descr_render(dsc));
		return error;
	}

	memset(attrbuf, 0, XFS_XATTR_LIST_MAX);
	memset(&cur, 0, sizeof(cur));
	memset(keybuf, 0, XATTR_NAME_MAX + 1);
	error = attr_list_by_handle(handle, sizeof(*handle), attrbuf,
			XFS_XATTR_LIST_MAX, attr_ns->flags, &cur);
	while (!error) {
		/* Examine the xattrs. */
		for (i = 0; i < attrlist->al_count; i++) {
			ent = ATTR_ENTRY(attrlist, i);
			snprintf(keybuf, XATTR_NAME_MAX, "%s.%s", attr_ns->name,
					ent->a_name);
			if (uc)
				error = unicrash_check_xattr_name(uc, dsc,
						keybuf);
			else
				error = simple_check_name(ctx, dsc,
						_("extended attribute"),
						keybuf);
			if (error) {
				str_liberror(ctx, error, descr_render(dsc));
				goto out;
			}
		}

		if (!attrlist->al_more)
			break;
		error = attr_list_by_handle(handle, sizeof(*handle), attrbuf,
				XFS_XATTR_LIST_MAX, attr_ns->flags, &cur);
	}
	if (error) {
		if (errno == ESTALE)
			errno = 0;
		error = errno;
		if (errno)
			str_errno(ctx, descr_render(dsc));
	}
out:
	unicrash_free(uc);
	return error;
}

/*
 * Check all the xattr names in all the xattr namespaces for problematic
 * characters.
 */
static int
check_xattr_names(
	struct scrub_ctx		*ctx,
	struct descr			*dsc,
	struct xfs_handle		*handle,
	struct xfs_bulkstat		*bstat)
{
	const struct attrns_decode	*ns;
	int				ret;

	for (ns = attr_ns; ns->name; ns++) {
		ret = check_xattr_ns_names(ctx, dsc, handle, bstat, ns);
		if (ret)
			break;
	}
	return ret;
}
#else
# define check_xattr_names(c, d, h, b)	(0)
#endif /* HAVE_LIBATTR */

static int
render_ino_from_handle(
	struct scrub_ctx	*ctx,
	char			*buf,
	size_t			buflen,
	void			*data)
{
	struct xfs_bulkstat	*bstat = data;

	return scrub_render_ino_descr(ctx, buf, buflen, bstat->bs_ino,
			bstat->bs_gen, NULL);
}

/* Defer this inode until later. */
static inline int
defer_inode(
	struct ncheck_state	*ncs,
	uint64_t		ino)
{
	int			error;

	pthread_mutex_lock(&ncs->lock);
	if (!ncs->new_deferred) {
		error = -bitmap_alloc(&ncs->new_deferred);
		if (error)
			goto unlock;
	}
	error = -bitmap_set(ncs->new_deferred, ino, 1);
unlock:
	pthread_mutex_unlock(&ncs->lock);
	return error;
}

/*
 * Check the directory structure for problems that could cause open_by_handle
 * not to work.  Returns 0 for no problems; EADDRNOTAVAIL if the there are
 * problems that would prevent name checking.
 */
static int
check_dir_connection(
	struct scrub_ctx		*ctx,
	struct ncheck_state		*ncs,
	const struct xfs_bulkstat	*bstat)
{
	struct scrub_item		sri = { };
	int				error;

	/* The dirtree scrubber only works when parent pointers are enabled */
	if (!(ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_PARENT))
		return 0;

	scrub_item_init_file(&sri, bstat);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_DIRTREE);

	error = scrub_item_check_file(ctx, &sri, -1);
	if (error) {
		str_liberror(ctx, error, _("checking directory loops"));
		return error;
	}

	if (ncs->last_call)
		error = repair_file_corruption_now(ctx, &sri, -1);
	else
		error = repair_file_corruption(ctx, &sri, -1);
	if (error) {
		str_liberror(ctx, error, _("repairing directory loops"));
		return error;
	}

	/* No directory tree problems?  Clear this inode if it was deferred. */
	if (repair_item_count_needsrepair(&sri) == 0) {
		if (ncs->cur_deferred)
			ncs->fixed_something = true;
		return 0;
	}

	/* Don't defer anything during last call. */
	if (ncs->last_call)
		return 0;

	/* Directory tree structure problems exist; do not check names yet. */
	error = defer_inode(ncs, bstat->bs_ino);
	if (error)
		return error;

	return EADDRNOTAVAIL;
}

/*
 * Verify the connectivity of the directory tree.
 * We know that the kernel's open-by-handle function will try to reconnect
 * parents of an opened directory, so we'll accept that as sufficient.
 *
 * Check for potential Unicode collisions in names.
 */
static int
check_inode_names(
	struct scrub_ctx	*ctx,
	struct xfs_handle	*handle,
	struct xfs_bulkstat	*bstat,
	void			*arg)
{
	DEFINE_DESCR(dsc, ctx, render_ino_from_handle);
	struct ncheck_state	*ncs = arg;
	int			fd = -1;
	int			error = 0;
	int			err2;

	descr_set(&dsc, bstat);
	background_sleep();

	/*
	 * Try to fix directory loops before we have problems opening files by
	 * handle.
	 */
	if (S_ISDIR(bstat->bs_mode)) {
		error = check_dir_connection(ctx, ncs, bstat);
		if (error == EADDRNOTAVAIL) {
			error = 0;
			goto out;
		}
		if (error)
			goto err;
	}

	/* Warn about naming problems in xattrs. */
	if (bstat->bs_xflags & FS_XFLAG_HASATTR) {
		error = check_xattr_names(ctx, &dsc, handle, bstat);
		if (error)
			goto err;
	}

	/*
	 * Warn about naming problems in the directory entries.  Opening the
	 * dir by handle means the kernel will try to reconnect it to the root.
	 * If the reconnection fails due to corruption in the parents we get
	 * ESTALE, which is why we skip phase 5 if we found corruption.
	 */
	if (S_ISDIR(bstat->bs_mode)) {
		fd = scrub_open_handle(handle);
		if (fd < 0) {
			error = errno;
			if (error == ESTALE)
				return ESTALE;
			str_errno(ctx, descr_render(&dsc));
			goto err;
		}

		error = check_dirent_names(ctx, &dsc, &fd, bstat);
		if (error)
			goto err_fd;
	}

	progress_add(1);
err_fd:
	if (fd >= 0) {
		err2 = close(fd);
		if (err2)
			str_errno(ctx, descr_render(&dsc));
		if (!error && err2)
			error = err2;
	}
err:
	if (error)
		ncs->aborted = true;
out:
	if (!error && ncs->aborted)
		error = ECANCELED;

	return error;
}

/* Try to check_inode_names on a specific inode. */
static int
retry_deferred_inode(
	struct ncheck_state	*ncs,
	struct xfs_handle	*handle,
	uint64_t		ino)
{
	struct xfs_bulkstat	bstat;
	struct scrub_ctx	*ctx = ncs->ctx;
	unsigned int		flags = 0;
	int			error;

	error = -xfrog_bulkstat_single(&ctx->mnt, ino, flags, &bstat);
	if (error == ENOENT) {
		/* Directory is gone, mark it clear. */
		ncs->fixed_something = true;
		return 0;
	}
	if (error)
		return error;

	handle->ha_fid.fid_ino = bstat.bs_ino;
	handle->ha_fid.fid_gen = bstat.bs_gen;

	return check_inode_names(ncs->ctx, handle, &bstat, ncs);
}

/* Try to check_inode_names on a range of inodes from the bitmap. */
static int
retry_deferred_inode_range(
	uint64_t		ino,
	uint64_t		len,
	void			*arg)
{
	struct xfs_handle	handle = { };
	struct ncheck_state	*ncs = arg;
	struct scrub_ctx	*ctx = ncs->ctx;
	uint64_t		i;
	int			error;

	memcpy(&handle.ha_fsid, ctx->fshandle, sizeof(handle.ha_fsid));
	handle.ha_fid.fid_len = sizeof(xfs_fid_t) -
			sizeof(handle.ha_fid.fid_len);
	handle.ha_fid.fid_pad = 0;

	for (i = 0; i < len; i++) {
		error = retry_deferred_inode(ncs, &handle, ino + i);
		if (error)
			return error;
	}

	return 0;
}

/*
 * Try to check_inode_names on inodes that were deferred due to directory tree
 * problems until we stop making progress.
 */
static int
retry_deferred_inodes(
	struct scrub_ctx	*ctx,
	struct ncheck_state	*ncs)
{
	int			error;

	if  (!ncs->new_deferred)
		return 0;

	/*
	 * Try to repair things until we stop making forward progress or we
	 * don't observe any new corruptions.  During the loop, we do not
	 * complain about the corruptions that do not get fixed.
	 */
	do {
		ncs->cur_deferred = ncs->new_deferred;
		ncs->new_deferred = NULL;
		ncs->fixed_something = false;

		error = -bitmap_iterate(ncs->cur_deferred,
				retry_deferred_inode_range, ncs);
		if (error)
			return error;

		bitmap_free(&ncs->cur_deferred);
	} while (ncs->fixed_something && ncs->new_deferred);

	/*
	 * Try one last time to fix things, and complain about any problems
	 * that remain.
	 */
	if (!ncs->new_deferred)
		return 0;

	ncs->cur_deferred = ncs->new_deferred;
	ncs->new_deferred = NULL;
	ncs->last_call = true;

	error = -bitmap_iterate(ncs->cur_deferred,
			retry_deferred_inode_range, ncs);
	if (error)
		return error;

	bitmap_free(&ncs->cur_deferred);
	return 0;
}

#ifndef FS_IOC_GETFSLABEL
# define FSLABEL_MAX		256
# define FS_IOC_GETFSLABEL	_IOR(0x94, 49, char[FSLABEL_MAX])
#endif /* FS_IOC_GETFSLABEL */

static int
scrub_render_mountpoint(
	struct scrub_ctx	*ctx,
	char			*buf,
	size_t			buflen,
	void			*data)
{
	return snprintf(buf, buflen, _("%s"), ctx->mntpoint);
}

/*
 * Check the filesystem label for Unicode normalization problems or misleading
 * sequences.
 */
static int
check_fs_label(
	struct scrub_ctx		*ctx)
{
	DEFINE_DESCR(dsc, ctx, scrub_render_mountpoint);
	char				label[FSLABEL_MAX];
	struct unicrash			*uc = NULL;
	int				error;

	error = unicrash_fs_label_init(&uc, ctx);
	if (error) {
		str_liberror(ctx, error, descr_render(&dsc));
		return error;
	}

	descr_set(&dsc, NULL);

	/* Retrieve label; quietly bail if we don't support that. */
	error = ioctl(ctx->mnt.fd, FS_IOC_GETFSLABEL, &label);
	if (error) {
		if (errno != EOPNOTSUPP && errno != ENOTTY) {
			error = errno;
			perror(ctx->mntpoint);
		}
		goto out;
	}

	/* Ignore empty labels. */
	if (label[0] == 0)
		goto out;

	/* Otherwise check for weirdness. */
	if (uc)
		error = unicrash_check_fs_label(uc, &dsc, label);
	else
		error = simple_check_name(ctx, &dsc, _("filesystem label"),
				label);
	if (error)
		str_liberror(ctx, error, descr_render(&dsc));
out:
	unicrash_free(uc);
	return error;
}

struct fs_scan_item {
	struct scrub_item	sri;
	bool			*abortedp;
};

/* Run one full-fs scan scrubber in this thread. */
static void
fs_scan_worker(
	struct workqueue	*wq,
	xfs_agnumber_t		nr,
	void			*arg)
{
	struct timespec		tv;
	struct fs_scan_item	*item = arg;
	struct scrub_ctx	*ctx = wq->wq_ctx;
	int			ret;

	/*
	 * Delay each successive fs scan by a second so that the threads are
	 * less likely to contend on the inobt and inode buffers.
	 */
	if (nr) {
		tv.tv_sec = nr;
		tv.tv_nsec = 0;
		nanosleep(&tv, NULL);
	}

	ret = scrub_item_check(ctx, &item->sri);
	if (ret) {
		str_liberror(ctx, ret, _("checking fs scan metadata"));
		*item->abortedp = true;
		goto out;
	}

	ret = repair_item_completely(ctx, &item->sri);
	if (ret) {
		str_liberror(ctx, ret, _("repairing fs scan metadata"));
		*item->abortedp = true;
		goto out;
	}

out:
	free(item);
	return;
}

/* Queue one full-fs scan scrubber. */
static int
queue_fs_scan(
	struct workqueue	*wq,
	bool			*abortedp,
	xfs_agnumber_t		nr,
	unsigned int		scrub_type)
{
	struct fs_scan_item	*item;
	struct scrub_ctx	*ctx = wq->wq_ctx;
	int			ret;

	item = malloc(sizeof(struct fs_scan_item));
	if (!item) {
		ret = ENOMEM;
		str_liberror(ctx, ret, _("setting up fs scan"));
		return ret;
	}
	scrub_item_init_fs(&item->sri);
	scrub_item_schedule(&item->sri, scrub_type);
	item->abortedp = abortedp;

	ret = -workqueue_add(wq, fs_scan_worker, nr, item);
	if (ret)
		str_liberror(ctx, ret, _("queuing fs scan work"));

	return ret;
}

/* Run multiple full-fs scan scrubbers at the same time. */
static int
run_kernel_fs_scan_scrubbers(
	struct scrub_ctx	*ctx)
{
	struct workqueue	wq_fs_scan;
	unsigned int		nr_threads = scrub_nproc_workqueue(ctx);
	xfs_agnumber_t		nr = 0;
	bool			aborted = false;
	int			ret, ret2;

	ret = -workqueue_create(&wq_fs_scan, (struct xfs_mount *)ctx,
			nr_threads);
	if (ret) {
		str_liberror(ctx, ret, _("setting up fs scan workqueue"));
		return ret;
	}

	/*
	 * The nlinks scanner is much faster than quotacheck because it only
	 * walks directories, so we start it first.
	 */
	ret = queue_fs_scan(&wq_fs_scan, &aborted, nr, XFS_SCRUB_TYPE_NLINKS);
	if (ret)
		goto wait;

	if (nr_threads > 1)
		nr++;

	ret = queue_fs_scan(&wq_fs_scan, &aborted, nr,
			XFS_SCRUB_TYPE_QUOTACHECK);
	if (ret)
		goto wait;

wait:
	ret2 = -workqueue_terminate(&wq_fs_scan);
	if (ret2) {
		str_liberror(ctx, ret2, _("joining fs scan workqueue"));
		if (!ret)
			ret = ret2;
	}
	if (aborted && !ret)
		ret = ECANCELED;

	workqueue_destroy(&wq_fs_scan);
	return ret;
}

/* Check directory connectivity. */
int
phase5_func(
	struct scrub_ctx	*ctx)
{
	struct ncheck_state	ncs = { .ctx = ctx };
	int			ret;


	/*
	 * Check and fix anything that requires a full filesystem scan.  We do
	 * this after we've checked all inodes and repaired anything that could
	 * get in the way of a scan.
	 */
	ret = run_kernel_fs_scan_scrubbers(ctx);
	if (ret)
		return ret;

	if (ctx->corruptions_found || ctx->unfixable_errors) {
		str_info(ctx, ctx->mntpoint,
_("Filesystem has errors, skipping connectivity checks."));
		return 0;
	}

	ret = check_fs_label(ctx);
	if (ret)
		return ret;

	pthread_mutex_init(&ncs.lock, NULL);

	ret = scrub_scan_all_inodes(ctx, check_inode_names, &ncs);
	if (ret)
		goto out_lock;
	if (ncs.aborted) {
		ret = ECANCELED;
		goto out_lock;
	}

	ret = retry_deferred_inodes(ctx, &ncs);
	if (ret)
		goto out_lock;

	scrub_report_preen_triggers(ctx);
out_lock:
	pthread_mutex_destroy(&ncs.lock);
	if (ncs.new_deferred)
		bitmap_free(&ncs.new_deferred);
	if (ncs.cur_deferred)
		bitmap_free(&ncs.cur_deferred);
	return ret;
}

/* Estimate how much work we're going to do. */
int
phase5_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	*items = scrub_estimate_iscan_work(ctx);
	*nr_threads = scrub_nproc(ctx) * 2;
	*rshift = 0;
	return 0;
}
