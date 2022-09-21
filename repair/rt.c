// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "dinode.h"
#include "protos.h"
#include "err_protos.h"
#include "rt.h"

#define xfs_highbit64 libxfs_highbit64	/* for XFS_RTBLOCKLOG macro */

void
rtinit(xfs_mount_t *mp)
{
	unsigned long long	wordcnt;

	if (mp->m_sb.sb_rblocks == 0)
		return;

	/*
	 * Allocate buffers for formatting the collected rt free space
	 * information.  The rtbitmap buffer must be large enough to compare
	 * against any unused bytes in the last block of the file.
	 */
	wordcnt = libxfs_rtbitmap_wordcount(mp, mp->m_sb.sb_rextents);
	btmcompute = calloc(wordcnt, sizeof(union xfs_rtword_raw));
	if (!btmcompute)
		do_error(
	_("couldn't allocate memory for incore realtime bitmap.\n"));

	wordcnt = libxfs_rtsummary_wordcount(mp, mp->m_rsumlevels,
			mp->m_sb.sb_rbmblocks);
	sumcompute = calloc(wordcnt, sizeof(union xfs_suminfo_raw));
	if (!sumcompute)
		do_error(
	_("couldn't allocate memory for incore realtime summary info.\n"));
}

static inline void
set_rtword(
	struct xfs_mount	*mp,
	union xfs_rtword_raw	*word,
	xfs_rtword_t		value)
{
	word->old = value;
}

static inline void
inc_sumcount(
	struct xfs_mount	*mp,
	union xfs_suminfo_raw	*info,
	xfs_rtsumoff_t		index)
{
	union xfs_suminfo_raw	*p = info + index;

	p->old++;
}

/*
 * generate the real-time bitmap and summary info based on the
 * incore realtime extent map.
 */
int
generate_rtinfo(
	struct xfs_mount	*mp,
	union xfs_rtword_raw	*words,
	union xfs_suminfo_raw	*sumcompute)
{
	xfs_rtxnum_t	extno;
	xfs_rtxnum_t	start_ext;
	int		bitsperblock;
	int		bmbno;
	xfs_rtword_t	freebit;
	xfs_rtword_t	bits;
	int		start_bmbno;
	int		i;
	int		offs;
	int		log;
	int		len;
	int		in_extent;

	ASSERT(mp->m_rbmip == NULL);

	bitsperblock = mp->m_blockwsize << XFS_NBWORDLOG;
	extno = start_ext = 0;
	bmbno = in_extent = start_bmbno = 0;

	/*
	 * slower but simple, don't play around with trying to set
	 * things one word at a time, just set bit as required.
	 * Have to * track start and end (size) of each range of
	 * free extents to set the summary info properly.
	 */
	while (extno < mp->m_sb.sb_rextents)  {
		freebit = 1;
		set_rtword(mp, words, 0);
		bits = 0;
		for (i = 0; i < sizeof(xfs_rtword_t) * NBBY &&
				extno < mp->m_sb.sb_rextents; i++, extno++)  {
			if (get_rtbmap(extno) == XR_E_FREE)  {
				sb_frextents++;
				bits |= freebit;

				if (in_extent == 0) {
					start_ext = extno;
					start_bmbno = bmbno;
					in_extent = 1;
				}
			} else if (in_extent == 1) {
				len = (int) (extno - start_ext);
				log = XFS_RTBLOCKLOG(len);
				offs = xfs_rtsumoffs(mp, log, start_bmbno);
				inc_sumcount(mp, sumcompute, offs);
				in_extent = 0;
			}

			freebit <<= 1;
		}
		set_rtword(mp, words, bits);
		words++;

		if (extno % bitsperblock == 0)
			bmbno++;
	}
	if (in_extent == 1) {
		len = (int) (extno - start_ext);
		log = XFS_RTBLOCKLOG(len);
		offs = xfs_rtsumoffs(mp, log, start_bmbno);
		inc_sumcount(mp, sumcompute, offs);
	}

	if (mp->m_sb.sb_frextents != sb_frextents) {
		do_warn(_("sb_frextents %" PRIu64 ", counted %" PRIu64 "\n"),
				mp->m_sb.sb_frextents, sb_frextents);
	}

	return(0);
}

static void
check_rtfile_contents(
	struct xfs_mount	*mp,
	const char		*filename,
	xfs_ino_t		ino,
	void			*buf,
	xfs_fileoff_t		filelen)
{
	struct xfs_bmbt_irec	map;
	struct xfs_buf		*bp;
	struct xfs_inode	*ip;
	xfs_fileoff_t		bno = 0;
	int			error;

	error = -libxfs_iget(mp, NULL, ino, 0, &ip);
	if (error) {
		do_warn(_("unable to open %s file, err %d\n"), filename, error);
		return;
	}

	if (ip->i_disk_size != XFS_FSB_TO_B(mp, filelen)) {
		do_warn(_("expected %s file size %llu, found %llu\n"),
				filename,
				(unsigned long long)XFS_FSB_TO_B(mp, filelen),
				(unsigned long long)ip->i_disk_size);
	}

	while (bno < filelen)  {
		xfs_filblks_t	maplen;
		int		nmap = 1;

		/* Read up to 1MB at a time. */
		maplen = min(filelen - bno, XFS_B_TO_FSBT(mp, 1048576));
		error = -libxfs_bmapi_read(ip, bno, maplen, &map, &nmap, 0);
		if (error) {
			do_warn(_("unable to read %s mapping, err %d\n"),
					filename, error);
			break;
		}

		if (map.br_startblock == HOLESTARTBLOCK) {
			do_warn(_("hole in %s file at dblock 0x%llx\n"),
					filename, (unsigned long long)bno);
			break;
		}

		error = -libxfs_buf_read_uncached(mp->m_dev,
				XFS_FSB_TO_DADDR(mp, map.br_startblock),
				XFS_FSB_TO_BB(mp, map.br_blockcount),
				0, &bp, NULL);
		if (error) {
			do_warn(_("unable to read %s at dblock 0x%llx, err %d\n"),
					filename, (unsigned long long)bno, error);
			break;
		}

		if (memcmp(bp->b_addr, buf, mp->m_blockwsize << XFS_WORDLOG))
			do_warn(_("discrepancy in %s at dblock 0x%llx\n"),
					filename, (unsigned long long)bno);

		buf += XFS_FSB_TO_B(mp, map.br_blockcount);
		bno += map.br_blockcount;
		libxfs_buf_relse(bp);
	}

	libxfs_irele(ip);
}

void
check_rtbitmap(
	struct xfs_mount	*mp)
{
	if (need_rbmino)
		return;

	check_rtfile_contents(mp, "rtbitmap", mp->m_sb.sb_rbmino, btmcompute,
			mp->m_sb.sb_rbmblocks);
}

void
check_rtsummary(
	struct xfs_mount	*mp)
{
	if (need_rsumino)
		return;

	check_rtfile_contents(mp, "rtsummary", mp->m_sb.sb_rsumino, sumcompute,
			XFS_B_TO_FSB(mp, mp->m_rsumsize));
}
