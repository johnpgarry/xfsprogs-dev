// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __RCBAG_BTREE_H__
#define __RCBAG_BTREE_H__

struct xfs_buf;
struct xfs_btree_cur;
struct xfs_mount;

#define RCBAG_MAGIC	0x74826671	/* 'JRBG' */

struct rcbag_key {
	uint32_t	rbg_startblock;
	uint32_t	rbg_blockcount;
	uint64_t	rbg_ino;
};

struct rcbag_rec {
	uint32_t	rbg_startblock;
	uint32_t	rbg_blockcount;
	uint64_t	rbg_ino;
	uint64_t	rbg_refcount;
};

typedef __be64 rcbag_ptr_t;

/* reflinks only exist on crc enabled filesystems */
#define RCBAG_BLOCK_LEN	XFS_BTREE_LBLOCK_CRC_LEN

/*
 * Record, key, and pointer address macros for btree blocks.
 *
 * (note that some of these may appear unused, but they are used in userspace)
 */
#define RCBAG_REC_ADDR(block, index) \
	((struct rcbag_rec *) \
		((char *)(block) + RCBAG_BLOCK_LEN + \
		 (((index) - 1) * sizeof(struct rcbag_rec))))

#define RCBAG_KEY_ADDR(block, index) \
	((struct rcbag_key *) \
		((char *)(block) + RCBAG_BLOCK_LEN + \
		 ((index) - 1) * sizeof(struct rcbag_key)))

#define RCBAG_PTR_ADDR(block, index, maxrecs) \
	((rcbag_ptr_t *) \
		((char *)(block) + RCBAG_BLOCK_LEN + \
		 (maxrecs) * sizeof(struct rcbag_key) + \
		 ((index) - 1) * sizeof(rcbag_ptr_t)))

unsigned int rcbagbt_maxrecs(struct xfs_mount *mp, unsigned int blocklen,
		bool leaf);

unsigned long long rcbagbt_calc_size(unsigned long long nr_records);

unsigned int rcbagbt_maxlevels_possible(void);

int __init rcbagbt_init_cur_cache(void);
void rcbagbt_destroy_cur_cache(void);

struct xfbtree;
struct xfs_btree_cur *rcbagbt_mem_cursor(struct xfs_mount *mp,
		struct xfs_trans *tp, struct xfbtree *xfbtree);
int rcbagbt_mem_init(struct xfs_mount *mp, struct xfs_buftarg *target,
		struct xfbtree *xfbtree);

#endif /* __RCBAG_BTREE_H__ */
