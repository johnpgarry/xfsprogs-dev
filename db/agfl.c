// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "type.h"
#include "faddr.h"
#include "fprint.h"
#include "field.h"
#include "io.h"
#include "bit.h"
#include "output.h"
#include "init.h"
#include "agfl.h"
#include "libfrog/bitmap.h"

static int agfl_bno_size(void *obj, int startoff);
static int agfl_f(int argc, char **argv);
static void agfl_help(void);

static const cmdinfo_t agfl_cmd =
	{ "agfl", NULL, agfl_f, 0, -1, 1, N_("[agno] [-g nr] [-p nr]"),
	  N_("set address to agfl block"), agfl_help };

const field_t	agfl_hfld[] = { {
	"", FLDT_AGFL, OI(0), C1, 0, TYP_NONE, },
	{ NULL }
};

const field_t	agfl_crc_hfld[] = { {
	"", FLDT_AGFL_CRC, OI(0), C1, 0, TYP_NONE, },
	{ NULL }
};

#define	OFF(f)	bitize(offsetof(struct xfs_agfl, agfl_ ## f))
const field_t	agfl_flds[] = {
	{ "bno", FLDT_AGBLOCKNZ, OI(OFF(magicnum)), agfl_bno_size,
	  FLD_ARRAY|FLD_COUNT, TYP_DATA },
	{ NULL }
};

const field_t	agfl_crc_flds[] = {
	{ "magicnum", FLDT_UINT32X, OI(OFF(magicnum)), C1, 0, TYP_NONE },
	{ "seqno", FLDT_AGNUMBER, OI(OFF(seqno)), C1, 0, TYP_NONE },
	{ "uuid", FLDT_UUID, OI(OFF(uuid)), C1, 0, TYP_NONE },
	{ "lsn", FLDT_UINT64X, OI(OFF(lsn)), C1, 0, TYP_NONE },
	{ "crc", FLDT_CRC, OI(OFF(crc)), C1, 0, TYP_NONE },
	/* the bno array is after the actual structure */
	{ "bno", FLDT_AGBLOCKNZ, OI(bitize(sizeof(struct xfs_agfl))),
	  agfl_bno_size, FLD_ARRAY|FLD_COUNT, TYP_DATA },
	{ NULL }
};

static int
agfl_bno_size(
	void	*obj,
	int	startoff)
{
	return libxfs_agfl_size(mp);
}

static void
agfl_help(void)
{
	dbprintf(_(
"\n"
" set allocation group freelist\n"
"\n"
" Example:\n"
"\n"
" agfl 5"
"\n"
" Located in the fourth sector of each allocation group,\n"
" the agfl freelist for internal btree space allocation is maintained\n"
" for each allocation group.  This acts as a reserved pool of space\n"
" separate from the general filesystem freespace (not used for user data).\n"
"\n"
" -g quantity\tRemove this many blocks from the AGFL.\n"
" -p quantity\tAdd this many blocks to the AGFL.\n"
"\n"
));

}

struct dump_info {
	struct xfs_perag	*pag;
	bool			leak;
};

/* Return blocks freed from the AGFL to the free space btrees. */
static int
free_grabbed(
	uint64_t		start,
	uint64_t		length,
	void			*data)
{
	struct dump_info	*di = data;
	struct xfs_perag	*pag = di->pag;
	struct xfs_mount	*mp = pag->pag_mount;
	struct xfs_trans	*tp;
	struct xfs_buf		*agf_bp;
	int			error;

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0,
			&tp);
	if (error)
		return error;

	error = -libxfs_alloc_read_agf(pag, tp, 0, &agf_bp);
	if (error)
		goto out_cancel;

	error = -libxfs_free_extent(tp, pag, start, length, &XFS_RMAP_OINFO_AG,
			XFS_AG_RESV_AGFL);
	if (error)
		goto out_cancel;

	return -libxfs_trans_commit(tp);

out_cancel:
	libxfs_trans_cancel(tp);
	return error;
}

/* Report blocks freed from the AGFL. */
static int
dump_grabbed(
	uint64_t		start,
	uint64_t		length,
	void			*data)
{
	struct dump_info	*di = data;
	const char		*fmt;

	if (length == 1)
		fmt = di->leak ? _("agfl %u: leaked agbno %u\n") :
				 _("agfl %u: removed agbno %u\n");
	else
		fmt = di->leak ? _("agfl %u: leaked agbno %u-%u\n") :
				 _("agfl %u: removed agbno %u-%u\n");

	printf(fmt, di->pag->pag_agno, (unsigned int)start,
			(unsigned int)(start + length - 1));
	return 0;
}

/* Remove blocks from the AGFL. */
static int
agfl_get(
	struct xfs_perag	*pag,
	int			quantity)
{
	struct dump_info	di = {
		.pag		= pag,
		.leak		= quantity < 0,
	};
	struct xfs_agf		*agf;
	struct xfs_buf		*agf_bp;
	struct xfs_trans	*tp;
	struct bitmap		*grabbed;
	const unsigned int	agfl_size = libxfs_agfl_size(pag->pag_mount);
	unsigned int		i;
	int			error;

	if (!quantity)
		return 0;

	if (di.leak)
		quantity = -quantity;
	quantity = min(quantity, agfl_size);

	error = bitmap_alloc(&grabbed);
	if (error)
		goto out;

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, quantity, 0,
			0, &tp);
	if (error)
		goto out_bitmap;

	error = -libxfs_alloc_read_agf(pag, tp, 0, &agf_bp);
	if (error)
		goto out_cancel;

	agf = agf_bp->b_addr;
	quantity = min(quantity, be32_to_cpu(agf->agf_flcount));

	for (i = 0; i < quantity; i++) {
		xfs_agblock_t	agbno;

		error = -libxfs_alloc_get_freelist(pag, tp, agf_bp, &agbno, 0);
		if (error)
			goto out_cancel;

		if (agbno == NULLAGBLOCK) {
			error = ENOSPC;
			goto out_cancel;
		}

		error = bitmap_set(grabbed, agbno, 1);
		if (error)
			goto out_cancel;
	}

	error = -libxfs_trans_commit(tp);
	if (error)
		goto out_bitmap;

	error = bitmap_iterate(grabbed, dump_grabbed, &di);
	if (error)
		goto out_bitmap;

	if (!di.leak) {
		error = bitmap_iterate(grabbed, free_grabbed, &di);
		if (error)
			goto out_bitmap;
	}

	bitmap_free(&grabbed);
	return 0;

out_cancel:
	libxfs_trans_cancel(tp);
out_bitmap:
	bitmap_free(&grabbed);
out:
	if (error)
		printf(_("agfl %u: %s\n"), pag->pag_agno, strerror(error));
	return error;
}

/* Add blocks to the AGFL. */
static int
agfl_put(
	struct xfs_perag	*pag,
	int			quantity)
{
	struct xfs_alloc_arg	args = {
		.mp		= pag->pag_mount,
		.alignment	= 1,
		.minlen		= 1,
		.prod		= 1,
		.resv		= XFS_AG_RESV_AGFL,
		.oinfo		= XFS_RMAP_OINFO_AG,
	};
	struct xfs_buf		*agfl_bp;
	struct xfs_agf		*agf;
	struct xfs_trans	*tp;
	xfs_fsblock_t		target;
	const unsigned int	agfl_size = libxfs_agfl_size(pag->pag_mount);
	unsigned int		i;
	bool			eoag = quantity < 0;
	int			error;

	if (!quantity)
		return 0;

	if (eoag)
		quantity = -quantity;
	quantity = min(quantity, agfl_size);

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, quantity, 0,
			0, &tp);
	if (error)
		return error;
	args.tp = tp;

	error = -libxfs_alloc_read_agf(pag, tp, 0, &args.agbp);
	if (error)
		goto out_cancel;

	agf = args.agbp->b_addr;
	args.maxlen = min(quantity, agfl_size - be32_to_cpu(agf->agf_flcount));

	if (eoag)
		target = XFS_AGB_TO_FSB(pag->pag_mount, pag->pag_agno,
				be32_to_cpu(agf->agf_length) - 1);
	else
		target = XFS_AGB_TO_FSB(pag->pag_mount, pag->pag_agno, 0);

	error = -libxfs_alloc_read_agfl(pag, tp, &agfl_bp);
	if (error)
		goto out_cancel;

	error = -libxfs_alloc_vextent_near_bno(&args, target);
	if (error)
		goto out_cancel;

	if (args.agbno == NULLAGBLOCK) {
		error = ENOSPC;
		goto out_cancel;
	}

	for (i = 0; i < args.len; i++) {
		error = -libxfs_alloc_put_freelist(pag, tp, args.agbp,
				agfl_bp, args.agbno + i, 0);
		if (error)
			goto out_cancel;
	}

	if (i == 1)
		printf(_("agfl %u: added agbno %u\n"), pag->pag_agno,
				args.agbno);
	else if (i > 1)
		printf(_("agfl %u: added agbno %u-%u\n"), pag->pag_agno,
				args.agbno, args.agbno + i - 1);

	error = -libxfs_trans_commit(tp);
	if (error)
		goto out;

	return 0;

out_cancel:
	libxfs_trans_cancel(tp);
out:
	if (error)
		printf(_("agfl %u: %s\n"), pag->pag_agno, strerror(error));
	return error;
}

static void
agfl_adjust(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	int			gblocks,
	int			pblocks)
{
	struct xfs_perag	*pag;
	int			error;

	if (!expert_mode) {
		printf(_("AGFL get/put only supported in expert mode.\n"));
		exitcode = 1;
		return;
	}

	pag = libxfs_perag_get(mp, agno);

	error = agfl_get(pag, gblocks);
	if (error)
		goto out_pag;

	error = agfl_put(pag, pblocks);

out_pag:
	libxfs_perag_put(pag);
	if (error)
		exitcode = 1;
}

static int
agfl_f(
	int		argc,
	char		**argv)
{
	xfs_agnumber_t	agno;
	char		*p;
	int		c;
	int		gblocks = 0, pblocks = 0;

	while ((c = getopt(argc, argv, "g:p:")) != -1) {
		switch (c) {
		case 'g':
			gblocks = atoi(optarg);
			break;
		case 'p':
			pblocks = atoi(optarg);
			break;
		default:
			agfl_help();
			return 1;
		}
	}

	if (argc > optind) {
		agno = (xfs_agnumber_t)strtoul(argv[optind], &p, 0);
		if (*p != '\0' || agno >= mp->m_sb.sb_agcount) {
			dbprintf(_("bad allocation group number %s\n"), argv[1]);
			return 0;
		}
		cur_agno = agno;
	} else if (cur_agno == NULLAGNUMBER)
		cur_agno = 0;

	if (gblocks || pblocks)
		agfl_adjust(mp, cur_agno, gblocks, pblocks);

	ASSERT(typtab[TYP_AGFL].typnm == TYP_AGFL);
	set_cur(&typtab[TYP_AGFL],
		XFS_AG_DADDR(mp, cur_agno, XFS_AGFL_DADDR(mp)),
		XFS_FSS_TO_BB(mp, 1), DB_RING_ADD, NULL);
	return 0;
}

void
agfl_init(void)
{
	add_command(&agfl_cmd);
}

/*ARGSUSED*/
int
agfl_size(
	void	*obj,
	int	startoff,
	int	idx)
{
	return bitize(mp->m_sb.sb_sectsize);
}
