// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef __LIBXFS_IO_H_
#define __LIBXFS_IO_H_

/*
 * Kernel equivalent buffer based I/O interface
 */

struct xfs_buf;
struct xfs_mount;
struct xfs_perag;

/*
 * IO verifier callbacks need the xfs_mount pointer, so we have to behave
 * somewhat like the kernel now for userspace IO in terms of having buftarg
 * based devices...
 */
struct xfs_buftarg {
	struct xfs_mount	*bt_mount;
	dev_t			dev;
	unsigned int		flags;
};

/* We purged a dirty buffer and lost a write. */
#define XFS_BUFTARG_LOST_WRITE		(1 << 0)
/* A dirty buffer failed the write verifier. */
#define XFS_BUFTARG_CORRUPT_WRITE	(1 << 1)

extern void	libxfs_buftarg_init(struct xfs_mount *mp, dev_t ddev,
				    dev_t logdev, dev_t rtdev);
int libxfs_blkdev_issue_flush(struct xfs_buftarg *btp);

#define LIBXFS_BBTOOFF64(bbs)	(((xfs_off_t)(bbs)) << BBSHIFT)

#define XB_PAGES        2
struct xfs_buf_map {
	xfs_daddr_t		bm_bn;  /* block number for I/O */
	int			bm_len; /* size of I/O */
};

#define DEFINE_SINGLE_BUF_MAP(map, blkno, numblk) \
	struct xfs_buf_map (map) = { .bm_bn = (blkno), .bm_len = (numblk) };

struct xfs_buf_ops {
	char *name;
	union {
		__be32 magic[2];	/* v4 and v5 on disk magic values */
		__be16 magic16[2];	/* v4 and v5 on disk magic values */
	};
	void (*verify_read)(struct xfs_buf *);
	void (*verify_write)(struct xfs_buf *);
	xfs_failaddr_t (*verify_struct)(struct xfs_buf *);
};

typedef struct xfs_buf {
	struct cache_node	b_node;
	unsigned int		b_flags;
	xfs_daddr_t		b_bn;
	unsigned		b_bcount;
	unsigned int		b_length;
	struct xfs_buftarg	*b_target;
#define b_dev		b_target->dev
	pthread_mutex_t		b_lock;
	pthread_t		b_holder;
	unsigned int		b_recur;
	void			*b_log_item;
	void			*b_transp;
	void			*b_addr;
	int			b_error;
	const struct xfs_buf_ops *b_ops;
	struct xfs_perag	*b_pag;
	struct xfs_mount	*b_mount;
	struct xfs_buf_map	*b_maps;
	struct xfs_buf_map	__b_map;
	int			b_nmaps;
	struct list_head	b_list;
#ifdef XFS_BUF_TRACING
	struct list_head	b_lock_list;
	const char		*b_func;
	const char		*b_file;
	int			b_line;
#endif
} xfs_buf_t;

bool xfs_verify_magic(struct xfs_buf *bp, __be32 dmagic);
bool xfs_verify_magic16(struct xfs_buf *bp, __be16 dmagic);

/* b_flags bits */
#define LIBXFS_B_DIRTY		0x0002	/* buffer has been modified */
#define LIBXFS_B_STALE		0x0004	/* buffer marked as invalid */
#define LIBXFS_B_UPTODATE	0x0008	/* buffer is sync'd to disk */
#define LIBXFS_B_DISCONTIG	0x0010	/* discontiguous buffer */
#define LIBXFS_B_UNCHECKED	0x0020	/* needs verification */

typedef unsigned int xfs_buf_flags_t;

#define XFS_BUF_DADDR_NULL		((xfs_daddr_t) (-1LL))

#define xfs_buf_offset(bp, offset)	((bp)->b_addr + (offset))
#define XFS_BUF_ADDR(bp)		((bp)->b_bn)
#define XFS_BUF_SIZE(bp)		((bp)->b_bcount)

#define XFS_BUF_SET_ADDR(bp,blk)	((bp)->b_bn = (blk))

#define XFS_BUF_SET_PRIORITY(bp,pri)	cache_node_set_priority( \
						libxfs_bcache, \
						&(bp)->b_node, \
						(pri))
#define XFS_BUF_PRIORITY(bp)		(cache_node_get_priority(&(bp)->b_node))
#define xfs_buf_set_ref(bp,ref)		((void) 0)
#define xfs_buf_ioerror(bp,err)		((bp)->b_error = (err))

#define xfs_daddr_to_agno(mp,d) \
	((xfs_agnumber_t)(XFS_BB_TO_FSBT(mp, d) / (mp)->m_sb.sb_agblocks))
#define xfs_daddr_to_agbno(mp,d) \
	((xfs_agblock_t)(XFS_BB_TO_FSBT(mp, d) % (mp)->m_sb.sb_agblocks))

/* Buffer Cache Interfaces */

extern struct cache	*libxfs_bcache;
extern struct cache_operations	libxfs_bcache_operations;

#define LIBXFS_GETBUF_TRYLOCK	(1 << 0)

#ifdef XFS_BUF_TRACING

#define libxfs_buf_read(dev, daddr, len, flags, ops) \
	libxfs_trace_readbuf(__FUNCTION__, __FILE__, __LINE__, \
			    (dev), (daddr), (len), (flags), (ops))
#define libxfs_readbuf_map(dev, map, nmaps, flags, ops) \
	libxfs_trace_readbuf_map(__FUNCTION__, __FILE__, __LINE__, \
			    (dev), (map), (nmaps), (flags), (ops))
#define libxfs_buf_mark_dirty(buf) \
	libxfs_trace_dirtybuf(__FUNCTION__, __FILE__, __LINE__, \
			      (buf))
#define libxfs_buf_get(dev, daddr, len) \
	libxfs_trace_getbuf(__FUNCTION__, __FILE__, __LINE__, \
			    (dev), (daddr), (len))
#define libxfs_getbuf_map(dev, map, nmaps, flags) \
	libxfs_trace_getbuf_map(__FUNCTION__, __FILE__, __LINE__, \
			    (dev), (map), (nmaps), (flags))
#define libxfs_buf_relse(buf) \
	libxfs_trace_putbuf(__FUNCTION__, __FILE__, __LINE__, (buf))

struct xfs_buf *libxfs_trace_readbuf(const char *func, const char *file,
			int line, struct xfs_buftarg *btp, xfs_daddr_t daddr,
			size_t len, int flags, const struct xfs_buf_ops *ops);
extern xfs_buf_t *libxfs_trace_readbuf_map(const char *, const char *, int,
			struct xfs_buftarg *, struct xfs_buf_map *, int, int,
			const struct xfs_buf_ops *);
void libxfs_trace_dirtybuf(const char *func, const char *file, int line,
			struct xfs_buf *bp);
struct xfs_buf *libxfs_trace_getbuf(const char *func, const char *file,
			int line, struct xfs_buftarg *btp, xfs_daddr_t daddr,
			size_t len);
extern xfs_buf_t *libxfs_trace_getbuf_map(const char *, const char *, int,
			struct xfs_buftarg *, struct xfs_buf_map *, int, int);
extern void	libxfs_trace_putbuf (const char *, const char *, int,
			xfs_buf_t *);

#else

extern xfs_buf_t *libxfs_readbuf_map(struct xfs_buftarg *, struct xfs_buf_map *,
			int, int, const struct xfs_buf_ops *);
void libxfs_buf_mark_dirty(struct xfs_buf *bp);
extern xfs_buf_t *libxfs_getbuf_map(struct xfs_buftarg *,
			struct xfs_buf_map *, int, int);
void	libxfs_buf_relse(struct xfs_buf *bp);

static inline struct xfs_buf*
libxfs_buf_get(
	struct xfs_buftarg	*target,
	xfs_daddr_t		blkno,
	size_t			numblks)
{
	DEFINE_SINGLE_BUF_MAP(map, blkno, numblks);

	return libxfs_getbuf_map(target, &map, 1, 0);
}

static inline struct xfs_buf*
libxfs_buf_read(
	struct xfs_buftarg	*target,
	xfs_daddr_t		blkno,
	size_t			numblks,
	xfs_buf_flags_t		flags,
	const struct xfs_buf_ops *ops)
{
	DEFINE_SINGLE_BUF_MAP(map, blkno, numblks);

	return libxfs_readbuf_map(target, &map, 1, flags, ops);
}

#endif /* XFS_BUF_TRACING */

int libxfs_readbuf_verify(struct xfs_buf *bp, const struct xfs_buf_ops *ops);
struct xfs_buf *libxfs_getsb(struct xfs_mount *mp);
extern void	libxfs_bcache_purge(void);
extern void	libxfs_bcache_free(void);
extern void	libxfs_bcache_flush(void);
extern int	libxfs_bcache_overflowed(void);

/* Buffer (Raw) Interfaces */
int		libxfs_bwrite(struct xfs_buf *bp);
extern int	libxfs_readbufr(struct xfs_buftarg *, xfs_daddr_t, xfs_buf_t *, int, int);
extern int	libxfs_readbufr_map(struct xfs_buftarg *, struct xfs_buf *, int);

extern int	libxfs_device_zero(struct xfs_buftarg *, xfs_daddr_t, uint);

extern int libxfs_bhash_size;

#define LIBXFS_BREAD	0x1
#define LIBXFS_BWRITE	0x2
#define LIBXFS_BZERO	0x4

extern void	libxfs_iomove (xfs_buf_t *, uint, int, void *, int);

static inline int
xfs_buf_verify_cksum(struct xfs_buf *bp, unsigned long cksum_offset)
{
	return xfs_verify_cksum(bp->b_addr, BBTOB(bp->b_length),
				cksum_offset);
}

static inline void
xfs_buf_update_cksum(struct xfs_buf *bp, unsigned long cksum_offset)
{
	xfs_update_cksum(bp->b_addr, BBTOB(bp->b_length),
			 cksum_offset);
}

static inline int
xfs_buf_associate_memory(struct xfs_buf *bp, void *mem, size_t len)
{
	bp->b_addr = mem;
	bp->b_bcount = len;
	return 0;
}

struct xfs_buf *libxfs_buf_get_uncached(struct xfs_buftarg *targ, size_t bblen,
		int flags);
int libxfs_buf_read_uncached(struct xfs_buftarg *targ, xfs_daddr_t daddr,
		size_t bblen, int flags, struct xfs_buf **bpp,
		const struct xfs_buf_ops *ops);

/* Push a single buffer on a delwri queue. */
static inline void
xfs_buf_delwri_queue(struct xfs_buf *bp, struct list_head *buffer_list)
{
	bp->b_node.cn_count++;
	list_add_tail(&bp->b_list, buffer_list);
}

int xfs_buf_delwri_submit(struct list_head *buffer_list);

#endif	/* __LIBXFS_IO_H__ */
