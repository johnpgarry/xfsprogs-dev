/*
 * Copyright (c) 2018 Red Hat, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"

static cmdinfo_t swapext_cmd;

static void
swapext_help(void)
{
	printf(_(
"\n"
" Swaps extents between the open file descriptor and the supplied filename.\n"
"\n"));
}

static int
xfs_bulkstat_single(
	int			fd,
	xfs_ino_t		*lastip,
	struct xfs_bstat	*ubuffer)
{
	struct xfs_fsop_bulkreq	bulkreq;

	bulkreq.lastip = (__u64 *)lastip;
	bulkreq.icount = 1;
	bulkreq.ubuffer = ubuffer;
	bulkreq.ocount = NULL;
	return ioctl(fd, XFS_IOC_FSBULKSTAT_SINGLE, &bulkreq);
}

static int
swapext_f(
	int			argc,
	char			**argv)
{
	int			fd;
	int			error;
	struct xfs_swapext	sx;
	struct stat		stat;

	/* open the donor file */
	fd = openfile(argv[1], NULL, 0, 0, NULL);
	if (fd < 0)
		return 0;

	/*
	 * stat the target file to get the inode number and use the latter to
	 * get the bulkstat info for the swapext cmd.
	 */
	error = fstat(file->fd, &stat);
	if (error) {
		perror("fstat");
		goto out;
	}

	error = xfs_bulkstat_single(file->fd, &stat.st_ino, &sx.sx_stat);
	if (error) {
		perror("bulkstat");
		goto out;
	}
	sx.sx_version = XFS_SX_VERSION;
	sx.sx_fdtarget = file->fd;
	sx.sx_fdtmp = fd;
	sx.sx_offset = 0;
	sx.sx_length = stat.st_size;
	error = ioctl(file->fd, XFS_IOC_SWAPEXT, &sx);
	if (error)
		perror("swapext");

out:
	close(fd);
	return 0;
}

void
swapext_init(void)
{
	swapext_cmd.name = "swapext";
	swapext_cmd.cfunc = swapext_f;
	swapext_cmd.argmin = 1;
	swapext_cmd.argmax = 1;
	swapext_cmd.flags = CMD_NOMAP_OK;
	swapext_cmd.args = _("<donorfile>");
	swapext_cmd.oneline = _("Swap extents between files.");
	swapext_cmd.help = swapext_help;

	add_command(&swapext_cmd);
}
