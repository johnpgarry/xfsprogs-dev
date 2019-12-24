// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000,2002,2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc.
 * All Rights Reserved.
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"

/*
 * Shortform directory ops
 */
static int
xfs_dir2_sf_entsize(
	struct xfs_dir2_sf_hdr	*hdr,
	int			len)
{
	int count = sizeof(struct xfs_dir2_sf_entry);	/* namelen + offset */

	count += len;					/* name */
	count += hdr->i8count ? XFS_INO64_SIZE : XFS_INO32_SIZE; /* ino # */
	return count;
}

static int
xfs_dir3_sf_entsize(
	struct xfs_dir2_sf_hdr	*hdr,
	int			len)
{
	return xfs_dir2_sf_entsize(hdr, len) + sizeof(uint8_t);
}

static struct xfs_dir2_sf_entry *
xfs_dir2_sf_nextentry(
	struct xfs_dir2_sf_hdr	*hdr,
	struct xfs_dir2_sf_entry *sfep)
{
	return (struct xfs_dir2_sf_entry *)
		((char *)sfep + xfs_dir2_sf_entsize(hdr, sfep->namelen));
}

static struct xfs_dir2_sf_entry *
xfs_dir3_sf_nextentry(
	struct xfs_dir2_sf_hdr	*hdr,
	struct xfs_dir2_sf_entry *sfep)
{
	return (struct xfs_dir2_sf_entry *)
		((char *)sfep + xfs_dir3_sf_entsize(hdr, sfep->namelen));
}


/*
 * For filetype enabled shortform directories, the file type field is stored at
 * the end of the name.  Because it's only a single byte, endian conversion is
 * not necessary. For non-filetype enable directories, the type is always
 * unknown and we never store the value.
 */
static uint8_t
xfs_dir2_sfe_get_ftype(
	struct xfs_dir2_sf_entry *sfep)
{
	return XFS_DIR3_FT_UNKNOWN;
}

static void
xfs_dir2_sfe_put_ftype(
	struct xfs_dir2_sf_entry *sfep,
	uint8_t			ftype)
{
	ASSERT(ftype < XFS_DIR3_FT_MAX);
}

static uint8_t
xfs_dir3_sfe_get_ftype(
	struct xfs_dir2_sf_entry *sfep)
{
	uint8_t		ftype;

	ftype = sfep->name[sfep->namelen];
	if (ftype >= XFS_DIR3_FT_MAX)
		return XFS_DIR3_FT_UNKNOWN;
	return ftype;
}

static void
xfs_dir3_sfe_put_ftype(
	struct xfs_dir2_sf_entry *sfep,
	uint8_t			ftype)
{
	ASSERT(ftype < XFS_DIR3_FT_MAX);

	sfep->name[sfep->namelen] = ftype;
}

/*
 * Inode numbers in short-form directories can come in two versions,
 * either 4 bytes or 8 bytes wide.  These helpers deal with the
 * two forms transparently by looking at the headers i8count field.
 *
 * For 64-bit inode number the most significant byte must be zero.
 */
static xfs_ino_t
xfs_dir2_sf_get_ino(
	struct xfs_dir2_sf_hdr	*hdr,
	uint8_t			*from)
{
	if (hdr->i8count)
		return get_unaligned_be64(from) & 0x00ffffffffffffffULL;
	else
		return get_unaligned_be32(from);
}

static void
xfs_dir2_sf_put_ino(
	struct xfs_dir2_sf_hdr	*hdr,
	uint8_t			*to,
	xfs_ino_t		ino)
{
	ASSERT((ino & 0xff00000000000000ULL) == 0);

	if (hdr->i8count)
		put_unaligned_be64(ino, to);
	else
		put_unaligned_be32(ino, to);
}

static xfs_ino_t
xfs_dir2_sf_get_parent_ino(
	struct xfs_dir2_sf_hdr	*hdr)
{
	return xfs_dir2_sf_get_ino(hdr, hdr->parent);
}

static void
xfs_dir2_sf_put_parent_ino(
	struct xfs_dir2_sf_hdr	*hdr,
	xfs_ino_t		ino)
{
	xfs_dir2_sf_put_ino(hdr, hdr->parent, ino);
}

/*
 * In short-form directory entries the inode numbers are stored at variable
 * offset behind the entry name. If the entry stores a filetype value, then it
 * sits between the name and the inode number. Hence the inode numbers may only
 * be accessed through the helpers below.
 */
static xfs_ino_t
xfs_dir2_sfe_get_ino(
	struct xfs_dir2_sf_hdr	*hdr,
	struct xfs_dir2_sf_entry *sfep)
{
	return xfs_dir2_sf_get_ino(hdr, &sfep->name[sfep->namelen]);
}

static void
xfs_dir2_sfe_put_ino(
	struct xfs_dir2_sf_hdr	*hdr,
	struct xfs_dir2_sf_entry *sfep,
	xfs_ino_t		ino)
{
	xfs_dir2_sf_put_ino(hdr, &sfep->name[sfep->namelen], ino);
}

static xfs_ino_t
xfs_dir3_sfe_get_ino(
	struct xfs_dir2_sf_hdr	*hdr,
	struct xfs_dir2_sf_entry *sfep)
{
	return xfs_dir2_sf_get_ino(hdr, &sfep->name[sfep->namelen + 1]);
}

static void
xfs_dir3_sfe_put_ino(
	struct xfs_dir2_sf_hdr	*hdr,
	struct xfs_dir2_sf_entry *sfep,
	xfs_ino_t		ino)
{
	xfs_dir2_sf_put_ino(hdr, &sfep->name[sfep->namelen + 1], ino);
}


/*
 * Directory data block operations
 */

/*
 * For special situations, the dirent size ends up fixed because we always know
 * what the size of the entry is. That's true for the "." and "..", and
 * therefore we know that they are a fixed size and hence their offsets are
 * constant, as is the first entry.
 *
 * Hence, this calculation is written as a macro to be able to be calculated at
 * compile time and so certain offsets can be calculated directly in the
 * structure initaliser via the macro. There are two macros - one for dirents
 * with ftype and without so there are no unresolvable conditionals in the
 * calculations. We also use round_up() as XFS_DIR2_DATA_ALIGN is always a power
 * of 2 and the compiler doesn't reject it (unlike roundup()).
 */
#define XFS_DIR2_DATA_ENTSIZE(n)					\
	round_up((offsetof(struct xfs_dir2_data_entry, name[0]) + (n) +	\
		 sizeof(xfs_dir2_data_off_t)), XFS_DIR2_DATA_ALIGN)

#define XFS_DIR3_DATA_ENTSIZE(n)					\
	round_up((offsetof(struct xfs_dir2_data_entry, name[0]) + (n) +	\
		 sizeof(xfs_dir2_data_off_t) + sizeof(uint8_t)),	\
		XFS_DIR2_DATA_ALIGN)

static int
xfs_dir2_data_entsize(
	int			n)
{
	return XFS_DIR2_DATA_ENTSIZE(n);
}

static int
xfs_dir3_data_entsize(
	int			n)
{
	return XFS_DIR3_DATA_ENTSIZE(n);
}

static uint8_t
xfs_dir2_data_get_ftype(
	struct xfs_dir2_data_entry *dep)
{
	return XFS_DIR3_FT_UNKNOWN;
}

static void
xfs_dir2_data_put_ftype(
	struct xfs_dir2_data_entry *dep,
	uint8_t			ftype)
{
	ASSERT(ftype < XFS_DIR3_FT_MAX);
}

static uint8_t
xfs_dir3_data_get_ftype(
	struct xfs_dir2_data_entry *dep)
{
	uint8_t		ftype = dep->name[dep->namelen];

	if (ftype >= XFS_DIR3_FT_MAX)
		return XFS_DIR3_FT_UNKNOWN;
	return ftype;
}

static void
xfs_dir3_data_put_ftype(
	struct xfs_dir2_data_entry *dep,
	uint8_t			type)
{
	ASSERT(type < XFS_DIR3_FT_MAX);
	ASSERT(dep->namelen != 0);

	dep->name[dep->namelen] = type;
}

/*
 * Pointer to an entry's tag word.
 */
static __be16 *
xfs_dir2_data_entry_tag_p(
	struct xfs_dir2_data_entry *dep)
{
	return (__be16 *)((char *)dep +
		xfs_dir2_data_entsize(dep->namelen) - sizeof(__be16));
}

static __be16 *
xfs_dir3_data_entry_tag_p(
	struct xfs_dir2_data_entry *dep)
{
	return (__be16 *)((char *)dep +
		xfs_dir3_data_entsize(dep->namelen) - sizeof(__be16));
}

/*
 * location of . and .. in data space (always block 0)
 */
static struct xfs_dir2_data_entry *
xfs_dir2_data_dot_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir2_data_hdr));
}

static struct xfs_dir2_data_entry *
xfs_dir2_data_dotdot_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR2_DATA_ENTSIZE(1));
}

static struct xfs_dir2_data_entry *
xfs_dir2_data_first_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR2_DATA_ENTSIZE(1) +
				XFS_DIR2_DATA_ENTSIZE(2));
}

static struct xfs_dir2_data_entry *
xfs_dir2_ftype_data_dotdot_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1));
}

static struct xfs_dir2_data_entry *
xfs_dir2_ftype_data_first_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1) +
				XFS_DIR3_DATA_ENTSIZE(2));
}

static struct xfs_dir2_data_entry *
xfs_dir3_data_dot_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir3_data_hdr));
}

static struct xfs_dir2_data_entry *
xfs_dir3_data_dotdot_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir3_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1));
}

static struct xfs_dir2_data_entry *
xfs_dir3_data_first_entry_p(
	struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir3_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1) +
				XFS_DIR3_DATA_ENTSIZE(2));
}

static struct xfs_dir2_data_free *
xfs_dir2_data_bestfree_p(struct xfs_dir2_data_hdr *hdr)
{
	return hdr->bestfree;
}

static struct xfs_dir2_data_free *
xfs_dir3_data_bestfree_p(struct xfs_dir2_data_hdr *hdr)
{
	return ((struct xfs_dir3_data_hdr *)hdr)->best_free;
}

static struct xfs_dir2_data_entry *
xfs_dir2_data_entry_p(struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir2_data_hdr));
}

static struct xfs_dir2_data_unused *
xfs_dir2_data_unused_p(struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_unused *)
		((char *)hdr + sizeof(struct xfs_dir2_data_hdr));
}

static struct xfs_dir2_data_entry *
xfs_dir3_data_entry_p(struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct xfs_dir3_data_hdr));
}

static struct xfs_dir2_data_unused *
xfs_dir3_data_unused_p(struct xfs_dir2_data_hdr *hdr)
{
	return (struct xfs_dir2_data_unused *)
		((char *)hdr + sizeof(struct xfs_dir3_data_hdr));
}

static const struct xfs_dir_ops xfs_dir2_ops = {
	.sf_entsize = xfs_dir2_sf_entsize,
	.sf_nextentry = xfs_dir2_sf_nextentry,
	.sf_get_ftype = xfs_dir2_sfe_get_ftype,
	.sf_put_ftype = xfs_dir2_sfe_put_ftype,
	.sf_get_ino = xfs_dir2_sfe_get_ino,
	.sf_put_ino = xfs_dir2_sfe_put_ino,
	.sf_get_parent_ino = xfs_dir2_sf_get_parent_ino,
	.sf_put_parent_ino = xfs_dir2_sf_put_parent_ino,

	.data_entsize = xfs_dir2_data_entsize,
	.data_get_ftype = xfs_dir2_data_get_ftype,
	.data_put_ftype = xfs_dir2_data_put_ftype,
	.data_entry_tag_p = xfs_dir2_data_entry_tag_p,
	.data_bestfree_p = xfs_dir2_data_bestfree_p,

	.data_dot_offset = sizeof(struct xfs_dir2_data_hdr),
	.data_dotdot_offset = sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR2_DATA_ENTSIZE(1),
	.data_first_offset =  sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR2_DATA_ENTSIZE(1) +
				XFS_DIR2_DATA_ENTSIZE(2),
	.data_entry_offset = sizeof(struct xfs_dir2_data_hdr),

	.data_dot_entry_p = xfs_dir2_data_dot_entry_p,
	.data_dotdot_entry_p = xfs_dir2_data_dotdot_entry_p,
	.data_first_entry_p = xfs_dir2_data_first_entry_p,
	.data_entry_p = xfs_dir2_data_entry_p,
	.data_unused_p = xfs_dir2_data_unused_p,
};

static const struct xfs_dir_ops xfs_dir2_ftype_ops = {
	.sf_entsize = xfs_dir3_sf_entsize,
	.sf_nextentry = xfs_dir3_sf_nextentry,
	.sf_get_ftype = xfs_dir3_sfe_get_ftype,
	.sf_put_ftype = xfs_dir3_sfe_put_ftype,
	.sf_get_ino = xfs_dir3_sfe_get_ino,
	.sf_put_ino = xfs_dir3_sfe_put_ino,
	.sf_get_parent_ino = xfs_dir2_sf_get_parent_ino,
	.sf_put_parent_ino = xfs_dir2_sf_put_parent_ino,

	.data_entsize = xfs_dir3_data_entsize,
	.data_get_ftype = xfs_dir3_data_get_ftype,
	.data_put_ftype = xfs_dir3_data_put_ftype,
	.data_entry_tag_p = xfs_dir3_data_entry_tag_p,
	.data_bestfree_p = xfs_dir2_data_bestfree_p,

	.data_dot_offset = sizeof(struct xfs_dir2_data_hdr),
	.data_dotdot_offset = sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1),
	.data_first_offset =  sizeof(struct xfs_dir2_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1) +
				XFS_DIR3_DATA_ENTSIZE(2),
	.data_entry_offset = sizeof(struct xfs_dir2_data_hdr),

	.data_dot_entry_p = xfs_dir2_data_dot_entry_p,
	.data_dotdot_entry_p = xfs_dir2_ftype_data_dotdot_entry_p,
	.data_first_entry_p = xfs_dir2_ftype_data_first_entry_p,
	.data_entry_p = xfs_dir2_data_entry_p,
	.data_unused_p = xfs_dir2_data_unused_p,
};

static const struct xfs_dir_ops xfs_dir3_ops = {
	.sf_entsize = xfs_dir3_sf_entsize,
	.sf_nextentry = xfs_dir3_sf_nextentry,
	.sf_get_ftype = xfs_dir3_sfe_get_ftype,
	.sf_put_ftype = xfs_dir3_sfe_put_ftype,
	.sf_get_ino = xfs_dir3_sfe_get_ino,
	.sf_put_ino = xfs_dir3_sfe_put_ino,
	.sf_get_parent_ino = xfs_dir2_sf_get_parent_ino,
	.sf_put_parent_ino = xfs_dir2_sf_put_parent_ino,

	.data_entsize = xfs_dir3_data_entsize,
	.data_get_ftype = xfs_dir3_data_get_ftype,
	.data_put_ftype = xfs_dir3_data_put_ftype,
	.data_entry_tag_p = xfs_dir3_data_entry_tag_p,
	.data_bestfree_p = xfs_dir3_data_bestfree_p,

	.data_dot_offset = sizeof(struct xfs_dir3_data_hdr),
	.data_dotdot_offset = sizeof(struct xfs_dir3_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1),
	.data_first_offset =  sizeof(struct xfs_dir3_data_hdr) +
				XFS_DIR3_DATA_ENTSIZE(1) +
				XFS_DIR3_DATA_ENTSIZE(2),
	.data_entry_offset = sizeof(struct xfs_dir3_data_hdr),

	.data_dot_entry_p = xfs_dir3_data_dot_entry_p,
	.data_dotdot_entry_p = xfs_dir3_data_dotdot_entry_p,
	.data_first_entry_p = xfs_dir3_data_first_entry_p,
	.data_entry_p = xfs_dir3_data_entry_p,
	.data_unused_p = xfs_dir3_data_unused_p,
};

/*
 * Return the ops structure according to the current config.  If we are passed
 * an inode, then that overrides the default config we use which is based on
 * feature bits.
 */
const struct xfs_dir_ops *
xfs_dir_get_ops(
	struct xfs_mount	*mp,
	struct xfs_inode	*dp)
{
	if (dp)
		return dp->d_ops;
	if (mp->m_dir_inode_ops)
		return mp->m_dir_inode_ops;
	if (xfs_sb_version_hascrc(&mp->m_sb))
		return &xfs_dir3_ops;
	if (xfs_sb_version_hasftype(&mp->m_sb))
		return &xfs_dir2_ftype_ops;
	return &xfs_dir2_ops;
}
