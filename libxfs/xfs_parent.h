// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2024 Oracle.
 * All Rights Reserved.
 */
#ifndef	__XFS_PARENT_H__
#define	__XFS_PARENT_H__

/* Metadata validators */
bool xfs_parent_namecheck(struct xfs_mount *mp,
		const struct xfs_parent_name_rec *rec, size_t reclen,
		unsigned int attr_flags);
bool xfs_parent_valuecheck(struct xfs_mount *mp, const void *value,
		size_t valuelen);
bool xfs_parent_hashcheck(struct xfs_mount *mp,
		const struct xfs_parent_name_rec *rec, const void *value,
		size_t valuelen);

extern struct kmem_cache	*xfs_parent_args_cache;

/*
 * Dynamically allocd structure used to wrap the needed data to pass around
 * the defer ops machinery
 */
struct xfs_parent_args {
	struct xfs_parent_name_rec	rec;
	struct xfs_parent_name_rec	new_rec;
	struct xfs_da_args		args;
};

int xfs_parent_args_alloc(struct xfs_mount *mp,
		struct xfs_parent_args **ppargsp);

/*
 * Initialize the parent pointer arguments structure.  Caller must have zeroed
 * the contents.
 */
static inline void
xfs_parent_args_init(
	struct xfs_mount		*mp,
	struct xfs_parent_args		*ppargs)
{
	ppargs->args.geo = mp->m_attr_geo;
	ppargs->args.whichfork = XFS_ATTR_FORK;
	ppargs->args.attr_filter = XFS_ATTR_PARENT;
	ppargs->args.op_flags = XFS_DA_OP_OKNOENT | XFS_DA_OP_LOGGED |
				XFS_DA_OP_NVLOOKUP;
	ppargs->args.name = (const uint8_t *)&ppargs->rec;
	ppargs->args.namelen = sizeof(struct xfs_parent_name_rec);
}

/*
 * Start a parent pointer update by allocating the context object we need to
 * perform a parent pointer update.
 */
static inline int
xfs_parent_start(
	struct xfs_mount	*mp,
	struct xfs_parent_args	**ppargsp)
{
	*ppargsp = NULL;

	if (xfs_has_parent(mp))
		return xfs_parent_args_alloc(mp, ppargsp);
	return 0;
}

int xfs_parent_addname(struct xfs_trans *tp, struct xfs_parent_args *ppargs,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child);

/* Schedule a parent pointer addition. */
static inline int
xfs_parent_add(struct xfs_trans *tp, struct xfs_parent_args *ppargs,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child)
{
	if (ppargs)
		return xfs_parent_addname(tp, ppargs, dp, parent_name, child);
	return 0;
}

int xfs_parent_removename(struct xfs_trans *tp, struct xfs_parent_args *ppargs,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child);

/* Schedule a parent pointer removal. */
static inline int
xfs_parent_remove(struct xfs_trans *tp, struct xfs_parent_args *ppargs,
		struct xfs_inode *dp, const struct xfs_name *parent_name,
		struct xfs_inode *child)
{
	if (ppargs)
		return xfs_parent_removename(tp, ppargs, dp, parent_name,
				child);
	return 0;
}

int xfs_parent_replacename(struct xfs_trans *tp,
		struct xfs_parent_args *ppargs,
		struct xfs_inode *old_dp, const struct xfs_name *old_name,
		struct xfs_inode *new_dp, const struct xfs_name *new_name,
		struct xfs_inode *child);

/* Schedule a parent pointer replacement. */
static inline int
xfs_parent_replace(struct xfs_trans *tp, struct xfs_parent_args *ppargs,
		struct xfs_inode *old_dp, const struct xfs_name *old_name,
		struct xfs_inode *new_dp, const struct xfs_name *new_name,
		struct xfs_inode *child)
{
	if (ppargs)
		return xfs_parent_replacename(tp, ppargs, old_dp, old_name,
				new_dp, new_name, child);
	return 0;
}

void xfs_parent_args_free(struct xfs_mount *mp, struct xfs_parent_args *ppargs);

/* Finish a parent pointer update by freeing the context object. */
static inline void
xfs_parent_finish(
	struct xfs_mount	*mp,
	struct xfs_parent_args	*ppargs)
{
	if (ppargs)
		xfs_parent_args_free(mp, ppargs);
}

/*
 * Incore version of a parent pointer, also contains dirent name so callers
 * can pass/obtain all the parent pointer information in a single structure
 */
struct xfs_parent_name_irec {
	/* Parent pointer attribute name fields */
	xfs_ino_t		p_ino;
	uint32_t		p_gen;
	xfs_dahash_t		p_namehash;

	/* Parent pointer attribute value fields */
	uint8_t			p_namelen;
	unsigned char		p_name[MAXNAMELEN];
};

void xfs_parent_irec_from_disk(struct xfs_parent_name_irec *irec,
		const struct xfs_parent_name_rec *rec, const void *value,
		unsigned int valuelen);
void xfs_parent_irec_to_disk(struct xfs_parent_name_rec *rec,
		const struct xfs_parent_name_irec *irec);
void xfs_parent_irec_hashname(struct xfs_mount *mp,
		struct xfs_parent_name_irec *irec);
bool xfs_parent_verify_irec(struct xfs_mount *mp,
		const struct xfs_parent_name_irec *irec);

/* Scratchpad memory so that raw parent operations don't burn stack space. */
struct xfs_parent_scratch {
	struct xfs_parent_name_rec	rec;
	struct xfs_da_args		args;
};

int xfs_parent_lookup(struct xfs_trans *tp, struct xfs_inode *ip,
		const struct xfs_parent_name_irec *pptr,
		struct xfs_parent_scratch *scratch);

#endif	/* __XFS_PARENT_H__ */
