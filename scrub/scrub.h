// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_SCRUB_H_
#define XFS_SCRUB_SCRUB_H_

enum xfrog_scrub_group;

/*
 * This flag boosts the repair priority of a scrub item when a dependent scrub
 * item is scheduled for repair.  Use a separate flag to preserve the
 * corruption state that we got from the kernel.  Priority boost is cleared the
 * next time xfs_repair_metadata is called.
 */
#define SCRUB_ITEM_BOOST_REPAIR	(1 << 0)

/*
 * These flags record the metadata object state that the kernel returned.
 * We want to remember if the object was corrupt, if the cross-referencing
 * revealed inconsistencies (xcorrupt), if the cross referencing itself failed
 * (xfail) or if the object is correct but could be optimised (preen).
 */
#define SCRUB_ITEM_CORRUPT	(XFS_SCRUB_OFLAG_CORRUPT)	/* (1 << 1) */
#define SCRUB_ITEM_PREEN	(XFS_SCRUB_OFLAG_PREEN)		/* (1 << 2) */
#define SCRUB_ITEM_XFAIL	(XFS_SCRUB_OFLAG_XFAIL)		/* (1 << 3) */
#define SCRUB_ITEM_XCORRUPT	(XFS_SCRUB_OFLAG_XCORRUPT)	/* (1 << 4) */

/* This scrub type needs to be checked. */
#define SCRUB_ITEM_NEEDSCHECK	(1 << 5)

/* Scrub barrier. */
#define SCRUB_ITEM_BARRIER	(1 << 6)

/* All of the state flags that we need to prioritize repair work. */
#define SCRUB_ITEM_REPAIR_ANY	(SCRUB_ITEM_CORRUPT | \
				 SCRUB_ITEM_PREEN | \
				 SCRUB_ITEM_XFAIL | \
				 SCRUB_ITEM_XCORRUPT)

/* Cross-referencing failures only. */
#define SCRUB_ITEM_REPAIR_XREF	(SCRUB_ITEM_XFAIL | \
				 SCRUB_ITEM_XCORRUPT)

/* Mask of bits signalling that a piece of metadata requires attention. */
#define SCRUB_ITEM_NEEDSREPAIR	(SCRUB_ITEM_CORRUPT | \
				 SCRUB_ITEM_XFAIL | \
				 SCRUB_ITEM_XCORRUPT)

/* Maximum number of times we'll retry a scrub ioctl call. */
#define SCRUB_ITEM_MAX_RETRIES	10

struct scrub_item {
	/*
	 * Information we need to call the scrub and repair ioctls.  Per-AG
	 * items should set the ino/gen fields to -1; per-inode items should
	 * set sri_agno to -1; and per-fs items should set all three fields to
	 * -1.  Or use the macros below.
	 */
	__u64			sri_ino;
	__u32			sri_gen;
	__u32			sri_agno;

	/* Bitmask of scrub types that were scheduled here. */
	__u64			sri_selected;

	/* Scrub item state flags, one for each XFS_SCRUB_TYPE. */
	__u8			sri_state[XFS_SCRUB_TYPE_NR];

	/* Track scrub and repair call retries for each scrub type. */
	__u8			sri_tries[XFS_SCRUB_TYPE_NR];

	/* Were there any corruption repairs needed? */
	bool			sri_inconsistent:1;

	/* Are we revalidating after repairs? */
	bool			sri_revalidate:1;
};

#define foreach_scrub_type(loopvar) \
	for ((loopvar) = 0; (loopvar) < XFS_SCRUB_TYPE_NR; (loopvar)++)

static inline void
scrub_item_init_ag(struct scrub_item *sri, xfs_agnumber_t agno)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = agno;
	sri->sri_ino = -1ULL;
	sri->sri_gen = -1U;
}

static inline void
scrub_item_init_rtgroup(struct scrub_item *sri, xfs_rgnumber_t rgno)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = rgno;
	sri->sri_ino = -1ULL;
	sri->sri_gen = -1U;
}

static inline void
scrub_item_init_fs(struct scrub_item *sri)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = -1U;
	sri->sri_ino = -1ULL;
	sri->sri_gen = -1U;
}

static inline void
scrub_item_init_file(struct scrub_item *sri, const struct xfs_bulkstat *bstat)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = -1U;
	sri->sri_ino = bstat->bs_ino;
	sri->sri_gen = bstat->bs_gen;
}

static inline void
scrub_item_init_metapath(struct scrub_item *sri, xfs_rgnumber_t rgno,
		uint64_t metapath)
{
	memset(sri, 0, sizeof(*sri));
	sri->sri_agno = rgno;
	sri->sri_ino = metapath;
}

void scrub_item_dump(struct scrub_item *sri, unsigned int group_mask,
		const char *tag);

static inline void
scrub_item_schedule(struct scrub_item *sri, unsigned int scrub_type)
{
	sri->sri_state[scrub_type] = SCRUB_ITEM_NEEDSCHECK;
	sri->sri_selected |= (1ULL << scrub_type);
}

void scrub_item_schedule_group(struct scrub_item *sri,
		enum xfrog_scrub_group group);
int scrub_item_check_file(struct scrub_ctx *ctx, struct scrub_item *sri,
		int override_fd);

static inline int
scrub_item_check(struct scrub_ctx *ctx, struct scrub_item *sri)
{
	return scrub_item_check_file(ctx, sri, -1);
}

/* Count the number of metadata objects still needing a scrub. */
static inline unsigned int
scrub_item_count_needscheck(
	const struct scrub_item		*sri)
{
	unsigned int			ret = 0;
	unsigned int			i;

	foreach_scrub_type(i)
		if (sri->sri_state[i] & SCRUB_ITEM_NEEDSCHECK)
			ret++;
	return ret;
}

void scrub_report_preen_triggers(struct scrub_ctx *ctx);

bool can_scrub_fs_metadata(struct scrub_ctx *ctx);
bool can_scrub_inode(struct scrub_ctx *ctx);
bool can_scrub_bmap(struct scrub_ctx *ctx);
bool can_scrub_dir(struct scrub_ctx *ctx);
bool can_scrub_attr(struct scrub_ctx *ctx);
bool can_scrub_symlink(struct scrub_ctx *ctx);
bool can_scrub_parent(struct scrub_ctx *ctx);
bool can_repair(struct scrub_ctx *ctx);
bool can_force_rebuild(struct scrub_ctx *ctx);

void check_scrubv(struct scrub_ctx *ctx);

int scrub_file(struct scrub_ctx *ctx, int fd, const struct xfs_bulkstat *bstat,
		unsigned int type, struct scrub_item *sri);

#endif /* XFS_SCRUB_SCRUB_H_ */
