// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "btree.h"
#include "err_protos.h"
#include "libxlog.h"
#include "incore.h"
#include "globals.h"
#include "dinode.h"
#include "slab.h"
#include "libfrog/bitmap.h"
#include "libfrog/platform.h"
#include "libxfs/xfile.h"
#include "libxfs/xfbtree.h"
#include "libxfs/xfs_btree_mem.h"
#include "rcbag_btree.h"
#include "rcbag.h"

struct rcbag {
	struct xfs_mount	*mp;
	struct xfbtree		xfbtree;
	uint64_t		nr_items;
};

int
rcbag_init(
	struct xfs_mount	*mp,
	uint64_t		max_rmaps,
	struct rcbag		**bagp)
{
	struct xfs_buftarg	*target;
	struct rcbag		*bag;
	char			*descr;
	unsigned long long	maxbytes;
	int			error;

	bag = calloc(1, sizeof(struct rcbag));
	if (!bag)
		return ENOMEM;

	bag->nr_items = 0;
	bag->mp = mp;

	/* Need to save space for the head block */
	maxbytes = (1 + rcbagbt_calc_size(max_rmaps)) * getpagesize();
	descr = kasprintf("xfs_repair (%s): refcount bag", mp->m_fsname);
	error = -xfile_alloc_buftarg(mp, descr, maxbytes, &target);
	kfree(descr);
	if (error)
		goto out_bag;

	error = rcbagbt_mem_init(mp, target, &bag->xfbtree);
	if (error)
		goto out_buftarg;

	*bagp = bag;
	return 0;

out_buftarg:
	xfile_free_buftarg(target);
out_bag:
	free(bag);
	return error;
}

void
rcbag_free(
	struct rcbag		**bagp)
{
	struct rcbag		*bag = *bagp;
	struct xfs_buftarg	*target = bag->xfbtree.target;

	xfbtree_destroy(&bag->xfbtree);
	xfile_free_buftarg(target);

	free(bag);
	*bagp = NULL;
}

/* Track an rmap in the refcount bag. */
void
rcbag_add(
	struct rcbag			*bag,
	const struct xfs_rmap_irec	*rmap)
{
	struct rcbag_rec		bagrec;
	struct xfs_mount		*mp = bag->mp;
	struct xfs_trans		*tp;
	struct xfs_btree_cur		*cur;
	int				has;
	int				error;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		do_error(_("allocating tx for refcount bag update\n"));

	cur = rcbagbt_mem_cursor(mp, tp, &bag->xfbtree);
	error = rcbagbt_lookup_eq(cur, rmap, &has);
	if (error)
		do_error(_("looking up refcount bag records\n"));

	if (has) {
		error = rcbagbt_get_rec(cur, &bagrec, &has);
		if (error || !has)
			do_error(_("reading refcount bag records\n"));

		bagrec.rbg_refcount++;
		error = rcbagbt_update(cur, &bagrec);
		if (error)
			do_error(_("updating refcount bag record\n"));
	} else {
		bagrec.rbg_startblock = rmap->rm_startblock;
		bagrec.rbg_blockcount = rmap->rm_blockcount;
		bagrec.rbg_ino = rmap->rm_owner;
		bagrec.rbg_refcount = 1;

		error = rcbagbt_insert(cur, &bagrec, &has);
		if (error || !has)
			do_error(_("adding refcount bag record, err %d\n"),
					error);
	}

	libxfs_btree_del_cursor(cur, error);

	error = -xfbtree_trans_commit(&bag->xfbtree, tp);
	if (error)
		do_error(_("committing refcount bag record\n"));

	libxfs_trans_cancel(tp);
	bag->nr_items++;
}

uint64_t
rcbag_count(
	const struct rcbag	*rcbag)
{
	return rcbag->nr_items;
}

#define BAGREC_NEXT(r)	((r)->rbg_startblock + (r)->rbg_blockcount)

/*
 * Find the next block where the refcount changes, given the next rmap we
 * looked at and the ones we're already tracking.
 */
void
rcbag_next_edge(
	struct rcbag			*bag,
	const struct xfs_rmap_irec	*next_rmap,
	bool				next_valid,
	uint32_t			*next_bnop)
{
	struct rcbag_rec		bagrec;
	struct xfs_mount		*mp = bag->mp;
	struct xfs_btree_cur		*cur;
	uint32_t			next_bno = NULLAGBLOCK;
	int				has;
	int				error;

	if (next_valid)
		next_bno = next_rmap->rm_startblock;

	cur = rcbagbt_mem_cursor(mp, NULL, &bag->xfbtree);
	error = -libxfs_btree_goto_left_edge(cur);
	if (error)
		do_error(_("seeking refcount bag btree cursor\n"));

	while (true) {
		error = -libxfs_btree_increment(cur, 0, &has);
		if (error)
			do_error(_("incrementing refcount bag btree cursor\n"));
		if (!has)
			break;

		error = rcbagbt_get_rec(cur, &bagrec, &has);
		if (error)
			do_error(_("reading refcount bag btree record\n"));
		if (!has)
			do_error(_("refcount bag btree record disappeared?\n"));

		next_bno = min(next_bno, BAGREC_NEXT(&bagrec));
	}

	/*
	 * We should have found /something/ because either next_rrm is the next
	 * interesting rmap to look at after emitting this refcount extent, or
	 * there are other rmaps in rmap_bag contributing to the current
	 * sharing count.  But if something is seriously wrong, bail out.
	 */
	if (next_bno == NULLAGBLOCK)
		do_error(_("next refcount bag edge not found?\n"));

	*next_bnop = next_bno;

	libxfs_btree_del_cursor(cur, error);
}

/* Pop all refcount bag records that end at next_bno */
void
rcbag_remove_ending_at(
	struct rcbag		*bag,
	uint32_t		next_bno)
{
	struct rcbag_rec	bagrec;
	struct xfs_mount	*mp = bag->mp;
	struct xfs_trans	*tp;
	struct xfs_btree_cur	*cur;
	int			has;
	int			error;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		do_error(_("allocating tx for refcount bag update\n"));

	/* go to the right edge of the tree */
	cur = rcbagbt_mem_cursor(mp, tp, &bag->xfbtree);
	memset(&cur->bc_rec, 0xFF, sizeof(cur->bc_rec));
	error = -libxfs_btree_lookup(cur, XFS_LOOKUP_GE, &has);
	if (error)
		do_error(_("seeking refcount bag btree cursor\n"));

	while (true) {
		error = -libxfs_btree_decrement(cur, 0, &has);
		if (error)
			do_error(_("decrementing refcount bag btree cursor\n"));
		if (!has)
			break;

		error = rcbagbt_get_rec(cur, &bagrec, &has);
		if (error)
			do_error(_("reading refcount bag btree record\n"));
		if (!has)
			do_error(_("refcount bag btree record disappeared?\n"));

		if (BAGREC_NEXT(&bagrec) != next_bno)
			continue;

		error = -libxfs_btree_delete(cur, &has);
		if (error)
			do_error(_("deleting refcount bag btree record, err %d\n"),
					error);
		if (!has)
			do_error(_("couldn't delete refcount bag record?\n"));

		bag->nr_items -= bagrec.rbg_refcount;
	}

	libxfs_btree_del_cursor(cur, error);

	error = -xfbtree_trans_commit(&bag->xfbtree, tp);
	if (error)
		do_error(_("committing refcount bag deletions\n"));

	libxfs_trans_cancel(tp);
}

/* Prepare to iterate the shared inodes tracked by the refcount bag. */
void
rcbag_ino_iter_start(
	struct rcbag		*bag,
	struct rcbag_iter	*iter)
{
	struct xfs_mount	*mp = bag->mp;
	int			error;

	memset(iter, 0, sizeof(struct rcbag_iter));

	if (bag->nr_items < 2)
		return;

	iter->cur = rcbagbt_mem_cursor(mp, NULL, &bag->xfbtree);
	error = -libxfs_btree_goto_left_edge(iter->cur);
	if (error)
		do_error(_("seeking refcount bag btree cursor\n"));
}

/* Tear down an iteration. */
void
rcbag_ino_iter_stop(
	struct rcbag		*bag,
	struct rcbag_iter	*iter)
{
	if (iter->cur)
		libxfs_btree_del_cursor(iter->cur, XFS_BTREE_NOERROR);
	iter->cur = NULL;
}

/*
 * Walk all the shared inodes tracked by the refcount bag.  Returns 1 when
 * returning a valid iter.ino, and 0 if iteration has completed.  The iter
 * should be initialized to zeroes before the first call.
 */
int
rcbag_ino_iter(
	struct rcbag		*bag,
	struct rcbag_iter	*iter)
{
	struct rcbag_rec	bagrec;
	int			has;
	int			error;

	if (bag->nr_items < 2)
		return 0;

	do {
		error = -libxfs_btree_increment(iter->cur, 0, &has);
		if (error)
			do_error(_("incrementing refcount bag btree cursor\n"));
		if (!has)
			return 0;

		error = rcbagbt_get_rec(iter->cur, &bagrec, &has);
		if (error)
			do_error(_("reading refcount bag btree record\n"));
		if (!has)
			do_error(_("refcount bag btree record disappeared?\n"));
	} while (iter->ino == bagrec.rbg_ino);

	iter->ino = bagrec.rbg_ino;
	return 1;
}

/* Dump the rcbag. */
void
rcbag_dump(
	struct rcbag			*bag)
{
	struct rcbag_rec		bagrec;
	struct xfs_mount		*mp = bag->mp;
	struct xfs_btree_cur		*cur;
	unsigned long long		nr = 0;
	int				has;
	int				error;

	cur = rcbagbt_mem_cursor(mp, NULL, &bag->xfbtree);
	error = -libxfs_btree_goto_left_edge(cur);
	if (error)
		do_error(_("seeking refcount bag btree cursor\n"));

	while (true) {
		error = -libxfs_btree_increment(cur, 0, &has);
		if (error)
			do_error(_("incrementing refcount bag btree cursor\n"));
		if (!has)
			break;

		error = rcbagbt_get_rec(cur, &bagrec, &has);
		if (error)
			do_error(_("reading refcount bag btree record\n"));
		if (!has)
			do_error(_("refcount bag btree record disappeared?\n"));

		printf("[%llu]: bno 0x%x fsbcount 0x%x ino 0x%llx refcount 0x%llx\n",
				nr++,
				(unsigned int)bagrec.rbg_startblock,
				(unsigned int)bagrec.rbg_blockcount,
				(unsigned long long)bagrec.rbg_ino,
				(unsigned long long)bagrec.rbg_refcount);
	}

	libxfs_btree_del_cursor(cur, error);
}
