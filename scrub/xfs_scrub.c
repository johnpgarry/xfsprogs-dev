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
#include "xfs_scrub.h"

/*
 * XFS Online Metadata Scrub (and Repair)
 *
 * The XFS scrubber uses custom XFS ioctls to probe more deeply into the
 * internals of the filesystem.  It takes advantage of scrubbing ioctls
 * to check all the records stored in a metadata object and to
 * cross-reference those records against the other filesystem metadata.
 *
 * After the program gathers command line arguments to figure out
 * exactly what the program is going to do, scrub execution is split up
 * into several separate phases:
 *
 * The "find geometry" phase queries XFS for the filesystem geometry.
 * The block devices for the data, realtime, and log devices are opened.
 * Kernel ioctls are test-queried to see if they actually work (the scrub
 * ioctl in particular), and any other filesystem-specific information
 * is gathered.
 *
 * In the "check internal metadata" phase, we call the metadata scrub
 * ioctl to check the filesystem's internal per-AG btrees.  This
 * includes the AG superblock, AGF, AGFL, and AGI headers, freespace
 * btrees, the regular and free inode btrees, the reverse mapping
 * btrees, and the reference counting btrees.  If the realtime device is
 * enabled, the realtime bitmap and reverse mapping btrees are checked.
 * Quotas, if enabled, are also checked in this phase.
 *
 * Each AG (and the realtime device) has its metadata checked in a
 * separate thread for better performance.  Errors in the internal
 * metadata can be fixed here prior to the inode scan; refer to the
 * section about the "repair filesystem" phase for more information.
 *
 * The "scan all inodes" phase uses BULKSTAT to scan all the inodes in
 * an AG in disk order.  The BULKSTAT information provides enough
 * information to construct a file handle that is used to check the
 * following parts of every file:
 *
 *  - The inode record
 *  - All three block forks (data, attr, CoW)
 *  - If it's a symlink, the symlink target.
 *  - If it's a directory, the directory entries.
 *  - All extended attributes
 *  - The parent pointer
 *
 * Multiple threads are started to check each the inodes of each AG in
 * parallel.  Errors in file metadata can be fixed here; see the section
 * about the "repair filesystem" phase for more information.
 *
 * Next comes the (configurable) "repair filesystem" phase.  The user
 * can instruct this program to fix all problems encountered; to fix
 * only optimality problems and leave the corruptions; or not to touch
 * the filesystem at all.  Any metadata repairs that did not succeed in
 * the previous two phases are retried here; if there are uncorrectable
 * errors, xfs_scrub stops here.
 *
 * The next phase is the "check directory tree" phase.  In this phase,
 * every directory is opened (via file handle) to confirm that each
 * directory is connected to the root.  Directory entries are checked
 * for ambiguous Unicode normalization mappings, which is to say that we
 * look for pairs of entries whose utf-8 strings normalize to the same
 * code point sequence and map to different inodes, because that could
 * be used to trick a user into opening the wrong file.  The names of
 * extended attributes are checked for Unicode normalization collisions.
 *
 * In the "verify data file integrity" phase, we employ GETFSMAP to read
 * the reverse-mappings of all AGs and issue direct-reads of the
 * underlying disk blocks.  We rely on the underlying storage to have
 * checksummed the data blocks appropriately.  Multiple threads are
 * started to check each AG in parallel; a separate thread pool is used
 * to handle the direct reads.
 *
 * In the "check summary counters" phase, use GETFSMAP to tally up the
 * blocks and BULKSTAT to tally up the inodes we saw and compare that to
 * the statfs output.  This gives the user a rough estimate of how
 * thorough the scrub was.
 */

/* Program name; needed for libfrog error reports. */
char				*progname = "xfs_scrub";

/* Debug level; higher values mean more verbosity. */
unsigned int			debug;

int
main(
	int			argc,
	char			**argv)
{
	fprintf(stdout, "EXPERIMENTAL xfs_scrub program in use! Use at your own risk!\n");
	return 4;
}
