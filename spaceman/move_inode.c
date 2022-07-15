// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libfrog/fsgeom.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "space.h"
#include "input.h"
#include "handle.h"
#include "relocation.h"

#include <linux/fiemap.h>
#include <linux/falloc.h>
#include <attr/attributes.h>

static cmdinfo_t move_inode_cmd;

/*
 * We can't entirely use O_TMPFILE here because we want to use RENAME_EXCHANGE
 * to swap the inode once rebuild is complete. Hence the new file has to be
 * somewhere in the namespace for rename to act upon. Hence we use a normal
 * open(O_CREATE) for now.
 *
 * This could potentially use O_TMPFILE to rebuild the entire inode, the use
 * a linkat()/renameat2() pair to add it to the namespace then atomically
 * replace the original.
 */
static int
create_tmpfile(
	const char	*mnt,
	struct xfs_fd	*xfd,
	xfs_agnumber_t	agno,
	char		**tmpfile,
	int		*tmpfd)
{
	char		name[PATH_MAX + 1];
	mode_t		mask;
	int		fd;
	int		i;
	int		ret;

	/* construct tmpdir */
	mask = umask(0);

	snprintf(name, PATH_MAX, "%s/.spaceman", mnt);
	ret = mkdir(name, 0700);
	if (ret) {
		if (errno != EEXIST) {
			fprintf(stderr, _("could not create tmpdir: %s: %s\n"),
					name, strerror(errno));
			ret = -errno;
			goto out_cleanup;
		}
	}

	/* loop creating directories until we get one in the right AG */
	for (i = 0; i < xfd->fsgeom.agcount; i++) {
		struct stat	st;

		snprintf(name, PATH_MAX, "%s/.spaceman/dir%d", mnt, i);
		ret = mkdir(name, 0700);
		if (ret) {
			if (errno != EEXIST) {
				fprintf(stderr,
					_("cannot create tmpdir: %s: %s\n"),
				       name, strerror(errno));
				ret = -errno;
				goto out_cleanup_dir;
			}
		}
		ret = lstat(name, &st);
		if (ret) {
			fprintf(stderr, _("cannot stat tmpdir: %s: %s\n"),
				       name, strerror(errno));
			ret = -errno;
			rmdir(name);
			goto out_cleanup_dir;
		}
		if (cvt_ino_to_agno(xfd, st.st_ino) == agno)
			break;

		/* remove directory in wrong AG */
		rmdir(name);
	}

	if (i == xfd->fsgeom.agcount) {
		/*
		 * Nothing landed in the selected AG! Must have been skipped
		 * because the AG is out of space.
		 */
		fprintf(stderr, _("Cannot create AG tmpdir.\n"));
		ret = -ENOSPC;
		goto out_cleanup_dir;
	}

	/* create tmpfile */
	snprintf(name, PATH_MAX, "%s/.spaceman/dir%d/tmpfile.%d", mnt, i, getpid());
	fd = open(name, O_CREAT|O_EXCL|O_RDWR, 0700);
	if (fd < 0) {
		fprintf(stderr, _("cannot create tmpfile: %s: %s\n"),
		       name, strerror(errno));
		ret = -errno;
	}

	/* return name and fd */
	(void)umask(mask);
	*tmpfd = fd;
	*tmpfile = strdup(name);

	return 0;
out_cleanup_dir:
	snprintf(name, PATH_MAX, "%s/.spaceman", mnt);
	rmdir(name);
out_cleanup:
	(void)umask(mask);
	return ret;
}

static int
get_attr(
	void		*hdl,
	size_t		hlen,
	char		*name,
	void		*attrbuf,
	int		*attrlen,
	int		attr_ns)
{
	struct xfs_attr_multiop	ops = {
		.am_opcode	= ATTR_OP_GET,
		.am_attrname	= name,
		.am_attrvalue	= attrbuf,
		.am_length	= *attrlen,
		.am_flags	= attr_ns,
	};
	int		ret;

	ret = attr_multi_by_handle(hdl, hlen, &ops, 1, 0);
	if (ret < 0) {
		fprintf(stderr, _("attr_multi_by_handle(GET): %s\n"),
			strerror(errno));
		return -errno;
	}
	*attrlen = ops.am_length;
	return 0;
}

static int
set_attr(
	void		*hdl,
	size_t		hlen,
	char		*name,
	void		*attrbuf,
	int		attrlen,
	int		attr_ns)
{
	struct xfs_attr_multiop	ops = {
		.am_opcode	= ATTR_OP_SET,
		.am_attrname	= name,
		.am_attrvalue	= attrbuf,
		.am_length	= attrlen,
		.am_flags	= ATTR_CREATE | attr_ns,
	};
	int		ret;

	ret = attr_multi_by_handle(hdl, hlen, &ops, 1, 0);
	if (ret < 0) {
		fprintf(stderr, _("attr_multi_by_handle(SET): %s\n"),
			strerror(errno));
		return -errno;
	}
	return 0;
}

/*
 * Copy all the attributes from the original source file into the replacement
 * destination.
 *
 * Oh the humanity of deprecated Irix compatible attr interfaces that are more
 * functional and useful than their native Linux replacements!
 */
static int
copy_attrs(
	int			srcfd,
	int			dstfd,
	int			attr_ns)
{
	void			*shdl;
	void			*dhdl;
	size_t			shlen;
	size_t			dhlen;
	attrlist_cursor_t	cursor;
	attrlist_t		*alist;
	struct attrlist_ent	*ent;
	char			alistbuf[XATTR_LIST_MAX];
	char			attrbuf[XATTR_SIZE_MAX];
	int			attrlen;
	int			error;
	int			i;

	memset(&cursor, 0, sizeof(cursor));

	/*
	 * All this handle based stuff is hoop jumping to avoid:
	 *
	 * a) deprecated API warnings because attr_list, attr_get and attr_set
	 *    have been deprecated hence through compiler warnings; and
	 *
	 * b) listxattr() failing hard if there are more than 64kB worth of attr
	 *    names on the inode so is unusable.
	 *
	 * That leaves libhandle as the only usable interface for iterating all
	 * xattrs on an inode reliably. Lucky for us, libhandle is part of
	 * xfsprogs, so this hoop jump isn't going to get ripped out from under
	 * us any time soon.
	 */
	error = fd_to_handle(srcfd, (void **)&shdl, &shlen);
	if (error) {
		fprintf(stderr, _("fd_to_handle(shdl): %s\n"),
			strerror(errno));
		return -errno;
	}
	error = fd_to_handle(dstfd, (void **)&dhdl, &dhlen);
	if (error) {
		fprintf(stderr, _("fd_to_handle(dhdl): %s\n"),
			strerror(errno));
		goto out_free_shdl;
	}

	/* loop to iterate all xattrs */
	error = attr_list_by_handle(shdl, shlen, alistbuf,
					XATTR_LIST_MAX, attr_ns, &cursor);
	if (error) {
		fprintf(stderr, _("attr_list_by_handle(shdl): %s\n"),
			strerror(errno));
	}
	while (!error) {
		alist = (attrlist_t *)alistbuf;

		/*
		 * We loop one attr at a time for initial implementation
		 * simplicity. attr_multi_by_handle() can retrieve and set
		 * multiple attrs in a single call, but that is more complex.
		 * Get it working first, then optimise.
		 */
		for (i = 0; i < alist->al_count; i++) {
			ent = ATTR_ENTRY(alist, i);

			/* get xattr (val, len) from name */
			attrlen = XATTR_SIZE_MAX;
			error = get_attr(shdl, shlen, ent->a_name, attrbuf,
						&attrlen, attr_ns);
			if (error)
				break;

			/* set xattr (val, len) to name */
			error = set_attr(dhdl, dhlen, ent->a_name, attrbuf,
						attrlen, ATTR_CREATE | attr_ns);
			if (error)
				break;
		}

		if (!alist->al_more)
			break;
		error = attr_list_by_handle(shdl, shlen, alistbuf,
					XATTR_LIST_MAX, attr_ns, &cursor);
	}

	free_handle(dhdl, dhlen);
out_free_shdl:
	free_handle(shdl, shlen);
	return error ? -errno : 0;
}

/*
 * scan the range of the new file for data that isn't in the destination AG
 * and unshare it to create a new copy of it in the current target location
 * of the new file.
 */
#define EXTENT_BATCH 32
static int
unshare_data(
	struct xfs_fd	*xfd,
	int		destfd,
	xfs_agnumber_t	agno)
{
	int		ret;
	struct fiemap	*fiemap;
	int		done = 0;
	int		fiemap_flags = FIEMAP_FLAG_SYNC;
	int		i;
	int		map_size;
	__u64		last_logical = 0;	/* last extent offset handled */
	off64_t		range_end = -1LL;	/* mapping end*/

	/* fiemap loop over extents */
	map_size = sizeof(struct fiemap) +
		(EXTENT_BATCH * sizeof(struct fiemap_extent));
	fiemap = malloc(map_size);
	if (!fiemap) {
		fprintf(stderr, _("%s: malloc of %d bytes failed.\n"),
			progname, map_size);
		return -ENOMEM;
	}

	while (!done) {
		memset(fiemap, 0, map_size);
		fiemap->fm_flags = fiemap_flags;
		fiemap->fm_start = last_logical;
		fiemap->fm_length = range_end - last_logical;
		fiemap->fm_extent_count = EXTENT_BATCH;

		ret = ioctl(destfd, FS_IOC_FIEMAP, (unsigned long)fiemap);
		if (ret < 0) {
			fprintf(stderr, "%s: ioctl(FS_IOC_FIEMAP): %s\n",
				progname, strerror(errno));
			free(fiemap);
			return -errno;
		}

		/* No more extents to map, exit */
		if (!fiemap->fm_mapped_extents)
			break;

		for (i = 0; i < fiemap->fm_mapped_extents; i++) {
			struct fiemap_extent	*extent;
			xfs_agnumber_t		this_agno;

			extent = &fiemap->fm_extents[i];
			this_agno = cvt_daddr_to_agno(xfd,
					cvt_btobbt(extent->fe_physical));

			/*
			 * If extent not in dst AG, unshare whole extent to
			 * trigger reallocated of the extent to be local to
			 * the current inode.
			 */
			if (this_agno != agno) {
				ret = fallocate(destfd, FALLOC_FL_UNSHARE_RANGE,
					extent->fe_logical, extent->fe_length);
				if (ret) {
					fprintf(stderr,
						"%s: fallocate(UNSHARE): %s\n",
						progname, strerror(errno));
					return -errno;
				}
			}

			last_logical = extent->fe_logical + extent->fe_length;

			/* Kernel has told us there are no more extents */
			if (extent->fe_flags & FIEMAP_EXTENT_LAST) {
				done = 1;
				break;
			}
		}
	}
	return 0;
}

/*
 * Exchange the inodes at the two paths indicated after first ensuring that the
 * owners, permissions and timestamps are set correctly in the tmpfile.
 */
static int
exchange_inodes(
	struct xfs_fd	*xfd,
	int		tmpfd,
	const char	*tmpfile,
	const char	*path)
{
	struct timespec	ts[2];
	struct stat	st;
	int		ret;

	ret = fstat(xfd->fd, &st);
	if (ret)
		return -errno;

	/* set user ids */
	ret = fchown(tmpfd, st.st_uid, st.st_gid);
	if (ret)
		return -errno;

	/* set permissions */
	ret = fchmod(tmpfd, st.st_mode);
	if (ret)
		return -errno;

	/* set timestamps */
	ts[0] = st.st_atim;
	ts[1] = st.st_mtim;
	ret = futimens(tmpfd, ts);
	if (ret)
		return -errno;

	/* exchange the two inodes */
	ret = renameat2(AT_FDCWD, tmpfile, AT_FDCWD, path, RENAME_EXCHANGE);
	if (ret)
		return -errno;
	return 0;
}

int
relocate_file_to_ag(
	const char		*mnt,
	const char		*path,
	struct xfs_fd		*xfd,
	xfs_agnumber_t		agno)
{
	int			ret;
	int			tmpfd = -1;
	char			*tmpfile = NULL;

	fprintf(stderr, "move mnt %s, path %s, agno %d\n", mnt, path, agno);

	/* create temporary file in agno */
	ret = create_tmpfile(mnt, xfd, agno, &tmpfile, &tmpfd);
	if (ret)
		return ret;

	/* clone data to tempfile */
	ret = ioctl(tmpfd, FICLONE, xfd->fd);
	if (ret)
		goto out_cleanup;

	/* copy system attributes to tempfile */
	ret = copy_attrs(xfd->fd, tmpfd, ATTR_ROOT);
	if (ret)
		goto out_cleanup;

	/* copy user attributes to tempfile */
	ret = copy_attrs(xfd->fd, tmpfd, 0);
	if (ret)
		goto out_cleanup;

	/* unshare data to move it */
	ret = unshare_data(xfd, tmpfd, agno);
	if (ret)
		goto out_cleanup;

	/* swap the inodes over */
	ret = exchange_inodes(xfd, tmpfd, tmpfile, path);

out_cleanup:
	if (ret == -1)
		ret = -errno;

	close(tmpfd);
	if (tmpfile)
		unlink(tmpfile);
	free(tmpfile);

	return ret;
}

static int
move_inode_f(
	int			argc,
	char			**argv)
{
	void			*fshandle;
	size_t			fshdlen;
	xfs_agnumber_t		agno = 0;
	struct stat		st;
	int			ret;
	int			c;

	while ((c = getopt(argc, argv, "a:")) != EOF) {
		switch (c) {
		case 'a':
			agno = cvt_u32(optarg, 10);
			if (errno) {
				fprintf(stderr, _("bad agno value %s\n"),
					optarg);
				return command_usage(&move_inode_cmd);
			}
			break;
		default:
			return command_usage(&move_inode_cmd);
		}
	}

	if (optind != argc)
		return command_usage(&move_inode_cmd);

	if (agno >= file->xfd.fsgeom.agcount) {
		fprintf(stderr,
_("Destination AG %d does not exist. Filesystem only has %d AGs\n"),
			agno, file->xfd.fsgeom.agcount);
		exitcode = 1;
		return 0;
	}

	/* this is so we can use fd_to_handle() later on */
	ret = path_to_fshandle(file->fs_path.fs_dir, &fshandle, &fshdlen);
	if (ret < 0) {
		fprintf(stderr, _("Cannot get fshandle for mount %s: %s\n"),
			file->fs_path.fs_dir, strerror(errno));
		goto exit_fail;
	}

	ret = fstat(file->xfd.fd, &st);
	if (ret) {
		fprintf(stderr, _("stat(%s) failed: %s\n"),
			file->name, strerror(errno));
		goto exit_fail;
	}

	if (S_ISREG(st.st_mode)) {
		ret = relocate_file_to_ag(file->fs_path.fs_dir, file->name,
				&file->xfd, agno);
	} else {
		fprintf(stderr, _("Unsupported: %s is not a regular file.\n"),
			file->name);
		goto exit_fail;
	}

	if (ret) {
		fprintf(stderr, _("Failed to move inode to AG %d: %s\n"),
			agno, strerror(-ret));
		goto exit_fail;
	}
	fshandle_destroy();
	return 0;

exit_fail:
	fshandle_destroy();
	exitcode = 1;
	return 0;
}

static void
move_inode_help(void)
{
	printf(_(
"\n"
"Physically move an inode into a new allocation group\n"
"\n"
" -a agno       -- destination AG agno for the current open file\n"
"\n"));

}

void
move_inode_init(void)
{
	move_inode_cmd.name = "move_inode";
	move_inode_cmd.altname = "mvino";
	move_inode_cmd.cfunc = move_inode_f;
	move_inode_cmd.argmin = 2;
	move_inode_cmd.argmax = 2;
	move_inode_cmd.args = "-a agno";
	move_inode_cmd.flags = CMD_FLAG_ONESHOT;
	move_inode_cmd.oneline = _("Move an inode into a new AG.");
	move_inode_cmd.help = move_inode_help;

	add_command(&move_inode_cmd);
}

