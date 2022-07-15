// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "command.h"
#include "fsmap.h"
#include "output.h"
#include "init.h"

struct fsmap_info {
	unsigned long long	nr;
	xfs_agnumber_t		agno;
};

static int
fsmap_fn(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct fsmap_info		*info = priv;

	dbprintf(_("%llu: %u/%u len %u owner %lld offset %llu bmbt %d attrfork %d extflag %d\n"),
		info->nr, info->agno, rec->rm_startblock,
		rec->rm_blockcount, rec->rm_owner, rec->rm_offset,
		!!(rec->rm_flags & XFS_RMAP_BMBT_BLOCK),
		!!(rec->rm_flags & XFS_RMAP_ATTR_FORK),
		!!(rec->rm_flags & XFS_RMAP_UNWRITTEN));
	info->nr++;

	return 0;
}

static void
fsmap(
	xfs_fsblock_t		start_fsb,
	xfs_fsblock_t		end_fsb)
{
	struct fsmap_info	info;
	xfs_agnumber_t		start_ag;
	xfs_agnumber_t		end_ag;
	xfs_daddr_t		eofs;
	struct xfs_rmap_irec	low = {0};
	struct xfs_rmap_irec	high = {0};
	struct xfs_btree_cur	*bt_cur;
	struct xfs_buf		*agbp;
	struct xfs_perag	*pag;
	int			error;

	eofs = XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks);
	if (XFS_FSB_TO_DADDR(mp, end_fsb) >= eofs)
		end_fsb = XFS_DADDR_TO_FSB(mp, eofs - 1);

	low.rm_startblock = XFS_FSB_TO_AGBNO(mp, start_fsb);
	high.rm_startblock = -1U;
	high.rm_owner = ULLONG_MAX;
	high.rm_offset = ULLONG_MAX;
	high.rm_flags = XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK | XFS_RMAP_UNWRITTEN;

	start_ag = XFS_FSB_TO_AGNO(mp, start_fsb);
	end_ag = XFS_FSB_TO_AGNO(mp, end_fsb);

	info.nr = 0;
	for_each_perag_range(mp, start_ag, end_ag, pag) {
		if (pag->pag_agno == end_ag)
			high.rm_startblock = XFS_FSB_TO_AGBNO(mp, end_fsb);

		error = -libxfs_alloc_read_agf(pag, NULL, 0, &agbp);
		if (error) {
			libxfs_perag_put(pag);
			dbprintf(_("Error %d while reading AGF.\n"), error);
			return;
		}

		bt_cur = libxfs_rmapbt_init_cursor(mp, NULL, agbp, pag);
		if (!bt_cur) {
			libxfs_buf_relse(agbp);
			libxfs_perag_put(pag);
			dbprintf(_("Not enough memory.\n"));
			return;
		}

		info.agno = pag->pag_agno;
		error = -libxfs_rmap_query_range(bt_cur, &low, &high,
				fsmap_fn, &info);
		if (error) {
			libxfs_btree_del_cursor(bt_cur, XFS_BTREE_ERROR);
			libxfs_buf_relse(agbp);
			libxfs_perag_put(pag);
			dbprintf(_("Error %d while querying fsmap btree.\n"),
				error);
			return;
		}

		libxfs_btree_del_cursor(bt_cur, XFS_BTREE_NOERROR);
		libxfs_buf_relse(agbp);

		if (pag->pag_agno == start_ag)
			low.rm_startblock = 0;
	}
}

static int
fsmap_rt_fn(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct fsmap_info		*info = priv;

	dbprintf(_("%llu: %u/%u len %u owner %lld offset %llu bmbt %d attrfork %d extflag %d\n"),
		info->nr, cur->bc_ino.rtg->rtg_rgno, rec->rm_startblock,
		rec->rm_blockcount, rec->rm_owner, rec->rm_offset,
		!!(rec->rm_flags & XFS_RMAP_BMBT_BLOCK),
		!!(rec->rm_flags & XFS_RMAP_ATTR_FORK),
		!!(rec->rm_flags & XFS_RMAP_UNWRITTEN));
	info->nr++;

	return 0;
}

static int
fsmap_rtgroup(
	struct xfs_rtgroup		*rtg,
	const struct xfs_rmap_irec	*low,
	const struct xfs_rmap_irec	*high,
	struct fsmap_info		*info)
{
	struct xfs_mount	*mp = rtg->rtg_mount;
	struct xfs_trans	*tp;
	struct xfs_inode	*ip;
	struct xfs_imeta_path	*path;
	struct xfs_btree_cur	*bt_cur;
	xfs_ino_t		ino;
	int			error;

	error = -libxfs_rtrmapbt_create_path(mp, rtg->rtg_rgno, &path);
	if (error) {
		dbprintf(
 _("Cannot create path to rtgroup %u rmap inode\n"),
				rtg->rtg_rgno);
		return error;
	}

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error) {
		dbprintf(
 _("Cannot alloc transaction to look up rtgroup %u rmap inode\n"),
				rtg->rtg_rgno);
		goto out_path;
	}
		
	error = -libxfs_imeta_lookup(tp, path, &ino);
	if (ino == NULLFSINO)
		error = ENOENT;
	if (error) {
		dbprintf(_("Cannot look up rtgroup %u rmap inode, error %d\n"),
				rtg->rtg_rgno, error);
		goto out_trans;
	}

	error = -libxfs_imeta_iget(tp, ino, XFS_DIR3_FT_REG_FILE, &ip);
	if (error) {
		dbprintf(_("Cannot load rtgroup %u rmap inode\n"),
				rtg->rtg_rgno);
		goto out_trans;
	}

	bt_cur = libxfs_rtrmapbt_init_cursor(mp, tp, rtg, ip);
	if (!bt_cur) {
		dbprintf(_("Not enough memory.\n"));
		goto out_rele;
	}

	error = -libxfs_rmap_query_range(bt_cur, low, high, fsmap_rt_fn,
			info);
	if (error) {
		dbprintf(_("Error %d while querying rt fsmap btree.\n"),
			error);
		goto out_cur;
	}

out_cur:
	libxfs_btree_del_cursor(bt_cur, error);
out_rele:
	libxfs_imeta_irele(ip);
out_trans:
	libxfs_trans_cancel(tp);
out_path:
	libxfs_imeta_free_path(path);
	return error;
}

static void
fsmap_rt(
	xfs_fsblock_t		start_fsb,
	xfs_fsblock_t		end_fsb)
{
	struct fsmap_info	info;
	xfs_daddr_t		eofs;
	struct xfs_rmap_irec	low;
	struct xfs_rmap_irec	high;
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		start_rg;
	xfs_rgnumber_t		end_rg;
	int			error;

	if (mp->m_sb.sb_rblocks == 0)
		return;

	eofs = XFS_FSB_TO_BB(mp, mp->m_sb.sb_rblocks);
	if (XFS_FSB_TO_DADDR(mp, end_fsb) >= eofs)
		end_fsb = XFS_DADDR_TO_FSB(mp, eofs - 1);

	low.rm_startblock = xfs_rtb_to_rgbno(mp, start_fsb, &start_rg);
	low.rm_owner = 0;
	low.rm_offset = 0;
	low.rm_flags = 0;
	high.rm_startblock = -1U;
	high.rm_owner = ULLONG_MAX;
	high.rm_offset = ULLONG_MAX;
	high.rm_flags = XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK |
			XFS_RMAP_UNWRITTEN;

	end_rg = xfs_rtb_to_rgno(mp, end_fsb);

	info.nr = 0;
	for_each_rtgroup_range(mp, start_rg, end_rg, rtg) {
		xfs_rgnumber_t		rgno;

		if (rtg->rtg_rgno == end_rg)
			high.rm_startblock = xfs_rtb_to_rgbno(mp, end_fsb,
					&rgno);
 
		error = fsmap_rtgroup(rtg, &low, &high, &info);
		if (error) {
			libxfs_rtgroup_put(rtg);
			return;
		}

		if (rtg->rtg_rgno == start_rg)
			low.rm_startblock = 0;
	}
}

static int
fsmap_f(
	int			argc,
	char			**argv)
{
	char			*p;
	int			c;
	xfs_fsblock_t		start_fsb = 0;
	xfs_fsblock_t		end_fsb = NULLFSBLOCK;
	bool			isrt = false;

	if (!xfs_has_rmapbt(mp)) {
		dbprintf(_("Filesystem does not support reverse mapping btree.\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "r")) != EOF) {
		switch (c) {
		case 'r':
			isrt = true;
			break;
		default:
			dbprintf(_("Bad option for fsmap command.\n"));
			return 0;
		}
	}

	if (argc > optind) {
		start_fsb = strtoull(argv[optind], &p, 0);
		if (*p != '\0' || start_fsb >= mp->m_sb.sb_dblocks) {
			dbprintf(_("Bad fsmap start_fsb %s.\n"), argv[optind]);
			return 0;
		}
	}

	if (argc > optind + 1) {
		end_fsb = strtoull(argv[optind + 1], &p, 0);
		if (*p != '\0') {
			dbprintf(_("Bad fsmap end_fsb %s.\n"), argv[optind + 1]);
			return 0;
		}
	}

	if (argc > optind + 2) {
		exitcode = 1;
		dbprintf(_("Too many arguments to fsmap.\n"));
		return 0;
	}

	if (isrt)
		fsmap_rt(start_fsb, end_fsb);
	else
		fsmap(start_fsb, end_fsb);

	return 0;
}

static const cmdinfo_t	fsmap_cmd =
	{ "fsmap", NULL, fsmap_f, 0, -1, 0,
	  N_("[-r] [start_fsb] [end_fsb]"),
	  N_("display reverse mapping(s)"), NULL };

void
fsmap_init(void)
{
	add_command(&fsmap_cmd);
}
