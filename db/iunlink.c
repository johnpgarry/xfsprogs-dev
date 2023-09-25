// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "command.h"
#include "output.h"
#include "init.h"

static xfs_filblks_t
count_rtblocks(
	struct xfs_inode	*ip)
{
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	got;
	xfs_filblks_t		count = 0;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	int			error;

	error = -libxfs_iread_extents(NULL, ip, XFS_DATA_FORK);
	if (error) {
		dbprintf(
_("could not read AG %u agino %u extents, err=%d\n"),
				XFS_INO_TO_AGNO(ip->i_mount, ip->i_ino),
				XFS_INO_TO_AGINO(ip->i_mount, ip->i_ino),
				error);
		return 0;
	}

	for_each_xfs_iext(ifp, &icur, &got)
		if (!isnullstartblock(got.br_startblock))
			count += got.br_blockcount;
	return count;
}

static xfs_agino_t
get_next_unlinked(
	xfs_agnumber_t		agno,
	xfs_agino_t		agino,
	bool			verbose)
{
	struct xfs_buf		*ino_bp;
	struct xfs_dinode	*dip;
	struct xfs_inode	*ip;
	xfs_ino_t		ino;
	xfs_agino_t		ret;
	int			error;

	ino = XFS_AGINO_TO_INO(mp, agno, agino);
	error = -libxfs_iget(mp, NULL, ino, 0, &ip);
	if (error)
		goto bad;

	if (verbose) {
		xfs_filblks_t	blocks, rtblks = 0;

		if (XFS_IS_REALTIME_INODE(ip))
			rtblks = count_rtblocks(ip);
		blocks = ip->i_nblocks - rtblks;

		dbprintf(_(" blocks %llu rtblocks %llu\n"),
				blocks, rtblks);
	} else {
		dbprintf("\n");
	}

	error = -libxfs_imap_to_bp(mp, NULL, &ip->i_imap, &ino_bp);
	if (error)
		goto bad;

	dip = xfs_buf_offset(ino_bp, ip->i_imap.im_boffset);
	ret = be32_to_cpu(dip->di_next_unlinked);
	libxfs_buf_relse(ino_bp);

	return ret;
bad:
	dbprintf(_("AG %u agino %u: %s\n"), agno, agino, strerror(error));
	return NULLAGINO;
}

static void
dump_unlinked_bucket(
	xfs_agnumber_t	agno,
	struct xfs_buf	*agi_bp,
	unsigned int	bucket,
	bool		quiet,
	bool		verbose)
{
	struct xfs_agi	*agi = agi_bp->b_addr;
	xfs_agino_t	agino;
	unsigned int	i = 0;

	agino = be32_to_cpu(agi->agi_unlinked[bucket]);
	if (agino != NULLAGINO)
		dbprintf(_("AG %u bucket %u agino %u"), agno, bucket, agino);
	else if (!quiet && agino == NULLAGINO)
		dbprintf(_("AG %u bucket %u agino NULL\n"), agno, bucket);

	while (agino != NULLAGINO) {
		agino = get_next_unlinked(agno, agino, verbose);
		if (agino != NULLAGINO)
			dbprintf(_("    [%u] agino %u"), i++, agino);
		else if (!quiet && agino == NULLAGINO)
			dbprintf(_("    [%u] agino NULL\n"), i++);
	}
}

static void
dump_unlinked(
	struct xfs_perag	*pag,
	unsigned int		bucket,
	bool			quiet,
	bool			verbose)
{
	struct xfs_buf		*agi_bp;
	xfs_agnumber_t		agno = pag->pag_agno;
	int			error;

	error = -libxfs_ialloc_read_agi(pag, NULL, &agi_bp);
	if (error) {
		dbprintf(_("AGI %u: %s\n"), agno, strerror(errno));
		return;
	}

	if (bucket != -1U) {
		dump_unlinked_bucket(agno, agi_bp, bucket, quiet, verbose);
		goto relse;
	}

	for (bucket = 0; bucket < XFS_AGI_UNLINKED_BUCKETS; bucket++) {
		dump_unlinked_bucket(agno, agi_bp, bucket, quiet, verbose);
	}

relse:
	libxfs_buf_relse(agi_bp);
}

static int
dump_iunlinked_f(
	int			argc,
	char			**argv)
{
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno = NULLAGNUMBER;
	unsigned int		bucket = -1U;
	bool			quiet = false;
	bool			verbose = false;
	int			c;

	while ((c = getopt(argc, argv, "a:b:qv")) != EOF) {
		switch (c) {
		case 'a':
			agno = atoi(optarg);
			if (agno >= mp->m_sb.sb_agcount) {
				dbprintf(_("Unknown AG %u, agcount is %u.\n"),
						agno, mp->m_sb.sb_agcount);
				return 0;
			}
			break;
		case 'b':
			bucket = atoi(optarg);
			if (bucket >= XFS_AGI_UNLINKED_BUCKETS) {
				dbprintf(_("Unknown bucket %u, max is 63.\n"),
						bucket);
				return 0;
			}
			break;
		case 'q':
			quiet = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			dbprintf(_("Bad option for dump_iunlinked command.\n"));
			return 0;
		}
	}

	if (agno != NULLAGNUMBER) {
		struct xfs_perag	*pag = libxfs_perag_get(mp, agno);

		dump_unlinked(pag, bucket, quiet, verbose);
		libxfs_perag_put(pag);
		return 0;
	}

	for_each_perag(mp, agno, pag)
		dump_unlinked(pag, bucket, quiet, verbose);

	return 0;
}

static const cmdinfo_t	dump_iunlinked_cmd =
	{ "dump_iunlinked", NULL, dump_iunlinked_f, 0, -1, 0,
	  N_("[-a agno] [-b bucket] [-q] [-v]"),
	  N_("dump chain of unlinked inode buckets"), NULL };

void
iunlink_init(void)
{
	add_command(&dump_iunlinked_cmd);
}
