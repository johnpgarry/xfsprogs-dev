// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2007, 2011 SGI
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libxlog.h"
#include "bmap.h"
#include "command.h"
#include "metadump.h"
#include "io.h"
#include "output.h"
#include "type.h"
#include "init.h"
#include "sig.h"
#include "xfs_metadump.h"
#include "fprint.h"
#include "faddr.h"
#include "field.h"
#include "dir2.h"
#include "obfuscate.h"

#undef REMAP_DEBUG

#ifdef REMAP_DEBUG
# define remap_debug		printf
#else
# define remap_debug(...)	((void)0)
#endif

#define DEFAULT_MAX_EXT_SIZE	XFS_MAX_BMBT_EXTLEN

/* copy all metadata structures to/from a file */

static int	metadump_f(int argc, char **argv);
static void	metadump_help(void);

/*
 * metadump commands issue info/wornings/errors to standard error as
 * metadump supports stdout as a destination.
 *
 * All static functions return zero on failure, while the public functions
 * return zero on success.
 */

static const cmdinfo_t	metadump_cmd =
	{ "metadump", NULL, metadump_f, 0, -1, 0,
		N_("[-a] [-e] [-g] [-m max_extent] [-w] [-o] [-v 1|2] filename"),
		N_("dump metadata to a file"), metadump_help };

struct metadump_ops {
	/*
	 * Initialize Metadump. This may perform actions such as
	 * 1. Allocating memory for structures required for dumping the
	 *    metadata.
	 * 2. Writing a header to the beginning of the metadump file.
	 */
	int (*init)(void);
	/*
	 * Write metadata to the metadump file along with the required ancillary
	 * data. @off and @len are in units of 512 byte blocks.
	 */
	int (*write)(enum typnm type, const char *data, xfs_daddr_t off,
			int len);
	/*
	 * Flush any in-memory remanents of metadata to the metadump file.
	 */
	int (*finish_dump)(void);
	/*
	 * Free resources allocated during metadump process.
	 */
	void (*release)(void);
};

static struct metadump {
	int			version;
	bool			show_progress;
	bool			stop_on_read_error;
	int			max_extent_size;
	bool			show_warnings;
	bool			obfuscate;
	bool			zero_stale_data;
	bool			progress_since_warning;
	bool			dirty_log;
	bool			external_log;
	bool			stdout_metadump;
	bool			realtime_data;
	xfs_ino_t		cur_ino;
	/* Metadump file */
	FILE			*outf;
	struct metadump_ops	*mdops;
	/* header + index + buffers */
	struct xfs_metablock	*metablock;
	__be64			*block_index;
	char			*block_buffer;
	int			num_indices;
	int			cur_index;
} metadump;

void
metadump_init(void)
{
	add_command(&metadump_cmd);
}

static void
metadump_help(void)
{
	dbprintf(_(
"\n"
" The 'metadump' command dumps the known metadata to a compact file suitable\n"
" for compressing and sending to an XFS maintainer for corruption analysis \n"
" or xfs_repair failures.\n\n"
" Options:\n"
"   -a -- Copy full metadata blocks without zeroing unused space\n"
"   -e -- Ignore read errors and keep going\n"
"   -g -- Display dump progress\n"
"   -m -- Specify max extent size in blocks to copy (default = %d blocks)\n"
"   -o -- Don't obfuscate names and extended attributes\n"
"   -v -- Metadump version to be used\n"
"   -w -- Show warnings of bad metadata information\n"
"\n"), DEFAULT_MAX_EXT_SIZE);
}

static void
print_warning(const char *fmt, ...)
{
	char		buf[200];
	va_list		ap;

	if (seenint())
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[sizeof(buf)-1] = '\0';

	fprintf(stderr, "%s%s: %s\n",
			metadump.progress_since_warning ? "\n" : "",
			progname, buf);
	metadump.progress_since_warning = false;
}

static void
print_progress(const char *fmt, ...)
{
	char		buf[60];
	va_list		ap;
	FILE		*f;

	if (seenint())
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[sizeof(buf)-1] = '\0';

	f = metadump.stdout_metadump ? stderr : stdout;
	fprintf(f, "\r%-59s", buf);
	fflush(f);
	metadump.progress_since_warning = true;
}

/*
 * we want to preserve the state of the metadata in the dump - whether it is
 * intact or corrupt, so even if the buffer has a verifier attached to it we
 * don't want to run it prior to writing the buffer to the metadump image.
 *
 * The only reason for running the verifier is to recalculate the CRCs on a
 * buffer that has been obfuscated. i.e. a buffer than metadump modified itself.
 * In this case, we only run the verifier if the buffer was not corrupt to begin
 * with so that we don't accidentally correct buffers with CRC or errors in them
 * when we are obfuscating them.
 */
static int
write_buf(
	iocur_t		*buf)
{
	struct xfs_buf	*bp = buf->bp;
	int		i;
	int		ret;

	/*
	 * Run the write verifier to recalculate the buffer CRCs and check
	 * metadump didn't introduce a new corruption. Warn if the verifier
	 * failed, but still continue to dump it into the output file.
	 */
	if (buf->need_crc && bp && bp->b_ops && !bp->b_error) {
		bp->b_ops->verify_write(bp);
		if (bp->b_error) {
			print_warning(
			    "obfuscation corrupted block at %s bno 0x%llx/0x%x",
				bp->b_ops->name,
				(long long)xfs_buf_daddr(bp), BBTOB(bp->b_length));
		}
	}

	/* handle discontiguous buffers */
	if (!buf->bbmap) {
		ret = metadump.mdops->write(buf->typ->typnm, buf->data, buf->bb,
				buf->blen);
		if (ret)
			return ret;
	} else {
		int	len = 0;
		for (i = 0; i < buf->bbmap->nmaps; i++) {
			ret = metadump.mdops->write(buf->typ->typnm,
					buf->data + BBTOB(len),
					buf->bbmap->b[i].bm_bn,
					buf->bbmap->b[i].bm_len);
			if (ret)
				return ret;
			len += buf->bbmap->b[i].bm_len;
		}
	}
	return seenint() ? -EINTR : 0;
}

/*
 * We could be processing a corrupt block, so we can't trust any of
 * the offsets or lengths to be within the buffer range. Hence check
 * carefully!
 */
static void
zero_btree_node(
	struct xfs_btree_block	*block,
	typnm_t			btype)
{
	int			nrecs;
	xfs_bmbt_ptr_t		*bpp;
	xfs_bmbt_key_t		*bkp;
	xfs_inobt_ptr_t		*ipp;
	xfs_inobt_key_t		*ikp;
	xfs_alloc_ptr_t		*app;
	xfs_alloc_key_t		*akp;
	char			*zp1, *zp2;
	char			*key_end;
	struct xfs_ino_geometry	*igeo = M_IGEO(mp);

	nrecs = be16_to_cpu(block->bb_numrecs);
	if (nrecs < 0)
		return;

	switch (btype) {
	case TYP_BMAPBTA:
	case TYP_BMAPBTD:
		if (nrecs > mp->m_bmap_dmxr[1])
			return;

		bkp = XFS_BMBT_KEY_ADDR(mp, block, 1);
		bpp = XFS_BMBT_PTR_ADDR(mp, block, 1, mp->m_bmap_dmxr[1]);
		zp1 = (char *)&bkp[nrecs];
		zp2 = (char *)&bpp[nrecs];
		key_end = (char *)bpp;
		break;
	case TYP_INOBT:
	case TYP_FINOBT:
		if (nrecs > igeo->inobt_mxr[1])
			return;

		ikp = XFS_INOBT_KEY_ADDR(mp, block, 1);
		ipp = XFS_INOBT_PTR_ADDR(mp, block, 1, igeo->inobt_mxr[1]);
		zp1 = (char *)&ikp[nrecs];
		zp2 = (char *)&ipp[nrecs];
		key_end = (char *)ipp;
		break;
	case TYP_BNOBT:
	case TYP_CNTBT:
		if (nrecs > mp->m_alloc_mxr[1])
			return;

		akp = XFS_ALLOC_KEY_ADDR(mp, block, 1);
		app = XFS_ALLOC_PTR_ADDR(mp, block, 1, mp->m_alloc_mxr[1]);
		zp1 = (char *)&akp[nrecs];
		zp2 = (char *)&app[nrecs];
		key_end = (char *)app;
		break;
	default:
		return;
	}


	/* Zero from end of keys to beginning of pointers */
	memset(zp1, 0, key_end - zp1);

	/* Zero from end of pointers to end of block */
	memset(zp2, 0, (char *)block + mp->m_sb.sb_blocksize - zp2);
}

/*
 * We could be processing a corrupt block, so we can't trust any of
 * the offsets or lengths to be within the buffer range. Hence check
 * carefully!
 */
static void
zero_btree_leaf(
	struct xfs_btree_block	*block,
	typnm_t			btype)
{
	int			nrecs;
	struct xfs_bmbt_rec	*brp;
	struct xfs_inobt_rec	*irp;
	struct xfs_alloc_rec	*arp;
	char			*zp;

	nrecs = be16_to_cpu(block->bb_numrecs);
	if (nrecs < 0)
		return;

	switch (btype) {
	case TYP_BMAPBTA:
	case TYP_BMAPBTD:
		if (nrecs > mp->m_bmap_dmxr[0])
			return;

		brp = XFS_BMBT_REC_ADDR(mp, block, 1);
		zp = (char *)&brp[nrecs];
		break;
	case TYP_INOBT:
	case TYP_FINOBT:
		if (nrecs > M_IGEO(mp)->inobt_mxr[0])
			return;

		irp = XFS_INOBT_REC_ADDR(mp, block, 1);
		zp = (char *)&irp[nrecs];
		break;
	case TYP_BNOBT:
	case TYP_CNTBT:
		if (nrecs > mp->m_alloc_mxr[0])
			return;

		arp = XFS_ALLOC_REC_ADDR(mp, block, 1);
		zp = (char *)&arp[nrecs];
		break;
	default:
		return;
	}

	/* Zero from end of records to end of block */
	memset(zp, 0, (char *)block + mp->m_sb.sb_blocksize - zp);
}

static void
zero_btree_block(
	struct xfs_btree_block	*block,
	typnm_t			btype)
{
	int			level;

	level = be16_to_cpu(block->bb_level);

	if (level > 0)
		zero_btree_node(block, btype);
	else
		zero_btree_leaf(block, btype);
}

static int
scan_btree(
	xfs_agnumber_t	agno,
	xfs_agblock_t	agbno,
	int		level,
	typnm_t		btype,
	void		*arg,
	int		(*func)(struct xfs_btree_block	*block,
				xfs_agnumber_t		agno,
				xfs_agblock_t		agbno,
				int			level,
				typnm_t			btype,
				void			*arg))
{
	int		rval = 0;

	push_cur();
	set_cur(&typtab[btype], XFS_AGB_TO_DADDR(mp, agno, agbno), blkbb,
			DB_RING_IGN, NULL);
	if (iocur_top->data == NULL) {
		print_warning("cannot read %s block %u/%u", typtab[btype].name,
				agno, agbno);
		rval = !metadump.stop_on_read_error;
		goto pop_out;
	}

	if (metadump.zero_stale_data) {
		zero_btree_block(iocur_top->data, btype);
		iocur_top->need_crc = 1;
	}

	if (write_buf(iocur_top))
		goto pop_out;

	if (!(*func)(iocur_top->data, agno, agbno, level - 1, btype, arg))
		goto pop_out;
	rval = 1;
pop_out:
	pop_cur();
	return rval;
}

/* free space tree copy routines */

static int
valid_bno(
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno)
{
	if (agno < (mp->m_sb.sb_agcount - 1) && agbno > 0 &&
			agbno <= mp->m_sb.sb_agblocks)
		return 1;
	if (agno == (mp->m_sb.sb_agcount - 1) && agbno > 0 &&
			agbno <= (mp->m_sb.sb_dblocks -
			 (xfs_rfsblock_t)(mp->m_sb.sb_agcount - 1) *
			 mp->m_sb.sb_agblocks))
		return 1;

	return 0;
}


static int
scanfunc_freesp(
	struct xfs_btree_block	*block,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	int			level,
	typnm_t			btype,
	void			*arg)
{
	xfs_alloc_ptr_t		*pp;
	int			i;
	int			numrecs;

	if (level == 0)
		return 1;

	numrecs = be16_to_cpu(block->bb_numrecs);
	if (numrecs > mp->m_alloc_mxr[1]) {
		if (metadump.show_warnings)
			print_warning("invalid numrecs (%u) in %s block %u/%u",
				numrecs, typtab[btype].name, agno, agbno);
		return 1;
	}

	pp = XFS_ALLOC_PTR_ADDR(mp, block, 1, mp->m_alloc_mxr[1]);
	for (i = 0; i < numrecs; i++) {
		if (!valid_bno(agno, be32_to_cpu(pp[i]))) {
			if (metadump.show_warnings)
				print_warning("invalid block number (%u/%u) "
					"in %s block %u/%u",
					agno, be32_to_cpu(pp[i]),
					typtab[btype].name, agno, agbno);
			continue;
		}
		if (!scan_btree(agno, be32_to_cpu(pp[i]), level, btype, arg,
				scanfunc_freesp))
			return 0;
	}
	return 1;
}

static int
copy_free_bno_btree(
	xfs_agnumber_t	agno,
	xfs_agf_t	*agf)
{
	xfs_agblock_t	root;
	int		levels;

	root = be32_to_cpu(agf->agf_roots[XFS_BTNUM_BNO]);
	levels = be32_to_cpu(agf->agf_levels[XFS_BTNUM_BNO]);

	/* validate root and levels before processing the tree */
	if (root == 0 || root > mp->m_sb.sb_agblocks) {
		if (metadump.show_warnings)
			print_warning("invalid block number (%u) in bnobt "
					"root in agf %u", root, agno);
		return 1;
	}
	if (levels > mp->m_alloc_maxlevels) {
		if (metadump.show_warnings)
			print_warning("invalid level (%u) in bnobt root "
					"in agf %u", levels, agno);
		return 1;
	}

	return scan_btree(agno, root, levels, TYP_BNOBT, agf, scanfunc_freesp);
}

static int
copy_free_cnt_btree(
	xfs_agnumber_t	agno,
	xfs_agf_t	*agf)
{
	xfs_agblock_t	root;
	int		levels;

	root = be32_to_cpu(agf->agf_roots[XFS_BTNUM_CNT]);
	levels = be32_to_cpu(agf->agf_levels[XFS_BTNUM_CNT]);

	/* validate root and levels before processing the tree */
	if (root == 0 || root > mp->m_sb.sb_agblocks) {
		if (metadump.show_warnings)
			print_warning("invalid block number (%u) in cntbt "
					"root in agf %u", root, agno);
		return 1;
	}
	if (levels > mp->m_alloc_maxlevels) {
		if (metadump.show_warnings)
			print_warning("invalid level (%u) in cntbt root "
					"in agf %u", levels, agno);
		return 1;
	}

	return scan_btree(agno, root, levels, TYP_CNTBT, agf, scanfunc_freesp);
}

static int
scanfunc_rmapbt(
	struct xfs_btree_block	*block,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	int			level,
	typnm_t			btype,
	void			*arg)
{
	xfs_rmap_ptr_t		*pp;
	int			i;
	int			numrecs;

	if (level == 0)
		return 1;

	numrecs = be16_to_cpu(block->bb_numrecs);
	if (numrecs > mp->m_rmap_mxr[1]) {
		if (metadump.show_warnings)
			print_warning("invalid numrecs (%u) in %s block %u/%u",
				numrecs, typtab[btype].name, agno, agbno);
		return 1;
	}

	pp = XFS_RMAP_PTR_ADDR(block, 1, mp->m_rmap_mxr[1]);
	for (i = 0; i < numrecs; i++) {
		if (!valid_bno(agno, be32_to_cpu(pp[i]))) {
			if (metadump.show_warnings)
				print_warning("invalid block number (%u/%u) "
					"in %s block %u/%u",
					agno, be32_to_cpu(pp[i]),
					typtab[btype].name, agno, agbno);
			continue;
		}
		if (!scan_btree(agno, be32_to_cpu(pp[i]), level, btype, arg,
				scanfunc_rmapbt))
			return 0;
	}
	return 1;
}

static int
copy_rmap_btree(
	xfs_agnumber_t	agno,
	struct xfs_agf	*agf)
{
	xfs_agblock_t	root;
	int		levels;

	if (!xfs_has_rmapbt(mp))
		return 1;

	root = be32_to_cpu(agf->agf_roots[XFS_BTNUM_RMAP]);
	levels = be32_to_cpu(agf->agf_levels[XFS_BTNUM_RMAP]);

	/* validate root and levels before processing the tree */
	if (root == 0 || root > mp->m_sb.sb_agblocks) {
		if (metadump.show_warnings)
			print_warning("invalid block number (%u) in rmapbt "
					"root in agf %u", root, agno);
		return 1;
	}
	if (levels > mp->m_rmap_maxlevels) {
		if (metadump.show_warnings)
			print_warning("invalid level (%u) in rmapbt root "
					"in agf %u", levels, agno);
		return 1;
	}

	return scan_btree(agno, root, levels, TYP_RMAPBT, agf, scanfunc_rmapbt);
}

static int
scanfunc_refcntbt(
	struct xfs_btree_block	*block,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	int			level,
	typnm_t			btype,
	void			*arg)
{
	xfs_refcount_ptr_t	*pp;
	int			i;
	int			numrecs;

	if (level == 0)
		return 1;

	numrecs = be16_to_cpu(block->bb_numrecs);
	if (numrecs > mp->m_refc_mxr[1]) {
		if (metadump.show_warnings)
			print_warning("invalid numrecs (%u) in %s block %u/%u",
				numrecs, typtab[btype].name, agno, agbno);
		return 1;
	}

	pp = XFS_REFCOUNT_PTR_ADDR(block, 1, mp->m_refc_mxr[1]);
	for (i = 0; i < numrecs; i++) {
		if (!valid_bno(agno, be32_to_cpu(pp[i]))) {
			if (metadump.show_warnings)
				print_warning("invalid block number (%u/%u) "
					"in %s block %u/%u",
					agno, be32_to_cpu(pp[i]),
					typtab[btype].name, agno, agbno);
			continue;
		}
		if (!scan_btree(agno, be32_to_cpu(pp[i]), level, btype, arg,
				scanfunc_refcntbt))
			return 0;
	}
	return 1;
}

static int
copy_refcount_btree(
	xfs_agnumber_t	agno,
	struct xfs_agf	*agf)
{
	xfs_agblock_t	root;
	int		levels;

	if (!xfs_has_reflink(mp))
		return 1;

	root = be32_to_cpu(agf->agf_refcount_root);
	levels = be32_to_cpu(agf->agf_refcount_level);

	/* validate root and levels before processing the tree */
	if (root == 0 || root > mp->m_sb.sb_agblocks) {
		if (metadump.show_warnings)
			print_warning("invalid block number (%u) in refcntbt "
					"root in agf %u", root, agno);
		return 1;
	}
	if (levels > mp->m_refc_maxlevels) {
		if (metadump.show_warnings)
			print_warning("invalid level (%u) in refcntbt root "
					"in agf %u", levels, agno);
		return 1;
	}

	return scan_btree(agno, root, levels, TYP_REFCBT, agf, scanfunc_refcntbt);
}

/* filename and extended attribute obfuscation routines */

struct name_ent {
	struct name_ent		*next;
	xfs_dahash_t		hash;
	int			namelen;
	unsigned char		name[1];
};

#define NAME_TABLE_SIZE		4096

static struct name_ent		*nametable[NAME_TABLE_SIZE];

static void
nametable_clear(void)
{
	int		i;
	struct name_ent	*ent;

	for (i = 0; i < NAME_TABLE_SIZE; i++) {
		while ((ent = nametable[i])) {
			nametable[i] = ent->next;
			free(ent);
		}
	}
}

/*
 * See if the given name is already in the name table.  If so,
 * return a pointer to its entry, otherwise return a null pointer.
 */
static struct name_ent *
nametable_find(xfs_dahash_t hash, int namelen, unsigned char *name)
{
	struct name_ent	*ent;

	for (ent = nametable[hash % NAME_TABLE_SIZE]; ent; ent = ent->next) {
		if (ent->hash == hash && ent->namelen == namelen &&
				!memcmp(ent->name, name, namelen))
			return ent;
	}
	return NULL;
}

/*
 * Add the given name to the name table.  Returns a pointer to the
 * name's new entry, or a null pointer if an error occurs.
 */
static struct name_ent *
nametable_add(xfs_dahash_t hash, int namelen, unsigned char *name)
{
	struct name_ent	*ent;

	ent = malloc(sizeof *ent + namelen);
	if (!ent)
		return NULL;

	ent->namelen = namelen;
	memcpy(ent->name, name, namelen);
	ent->hash = hash;
	ent->next = nametable[hash % NAME_TABLE_SIZE];

	nametable[hash % NAME_TABLE_SIZE] = ent;

	return ent;
}

/*
 * Obfuscated name remapping table for parent pointer-enabled filesystems.
 * When this feature is enabled, we have to maintain consistency between the
 * names that appears in the dirent and the corresponding parent pointer.
 */

struct remap_ent {
	struct remap_ent	*next;
	xfs_ino_t		dir_ino;
	xfs_dahash_t		namehash;
	uint8_t			namelen;

	uint8_t			names[];
};

static inline uint8_t *remap_ent_before(struct remap_ent *ent)
{
	return &ent->names[0];
}

static inline uint8_t *remap_ent_after(struct remap_ent *ent)
{
	return &ent->names[ent->namelen];
}

#define REMAP_TABLE_SIZE		4096

static struct remap_ent		*remaptable[REMAP_TABLE_SIZE];

static void
remaptable_clear(void)
{
	int			i;
	struct remap_ent	*ent, *next;

	for (i = 0; i < REMAP_TABLE_SIZE; i++) {
		ent = remaptable[i];

		while (ent) {
			next = ent->next;
			free(ent);
			ent = next;
		}
	}
}

/* Try to find a remapping table entry. */
static struct remap_ent *
remaptable_find(
	xfs_ino_t		dir_ino,
	xfs_dahash_t		namehash,
	const unsigned char	*name,
	unsigned int		namelen)
{
	struct remap_ent	*ent = remaptable[namehash % REMAP_TABLE_SIZE];

	remap_debug("REMAP FIND: 0x%lx hash 0x%x '%.*s'\n",
			dir_ino, namehash, namelen, name);

	while (ent) {
		remap_debug("REMAP ENT: 0x%lx hash 0x%x '%.*s'\n",
				ent->dir_ino, ent->namehash, ent->namelen,
				remap_ent_before(ent));

		if (ent->dir_ino == dir_ino &&
		    ent->namehash == namehash &&
		    ent->namelen == namelen &&
		    !memcmp(remap_ent_before(ent), name, namelen))
			return ent;
		ent = ent->next;
	}

	return NULL;
}

/* Remember the remapping for a particular dirent that we obfuscated. */
static struct remap_ent *
remaptable_add(
	xfs_ino_t		dir_ino,
	xfs_dahash_t		namehash,
	const unsigned char	*old_name,
	unsigned int		namelen,
	const unsigned char	*new_name)
{
	struct remap_ent	*ent;

	ent = malloc(sizeof(struct remap_ent) + (namelen * 2));
	if (!ent)
		return NULL;

	ent->dir_ino = dir_ino;
	ent->namehash = namehash;
	ent->namelen = namelen;
	memcpy(remap_ent_before(ent), old_name, namelen);
	memcpy(remap_ent_after(ent), new_name, namelen);
	ent->next = remaptable[namehash % REMAP_TABLE_SIZE];

	remaptable[namehash % REMAP_TABLE_SIZE] = ent;

	remap_debug("REMAP ADD: 0x%lx hash 0x%x '%.*s' -> '%.*s'\n",
			dir_ino, namehash, namelen, old_name, namelen,
			new_name);
	return ent;
}

#define	ORPHANAGE	"lost+found"
#define	ORPHANAGE_LEN	(sizeof (ORPHANAGE) - 1)

static inline int
is_orphanage_dir(
	struct xfs_mount	*mp,
	xfs_ino_t		dir_ino,
	size_t			name_len,
	unsigned char		*name)
{
	return dir_ino == mp->m_sb.sb_rootino &&
			name_len == ORPHANAGE_LEN &&
			!memcmp(name, ORPHANAGE, ORPHANAGE_LEN);
}

/*
 * Determine whether a name is one we shouldn't obfuscate because
 * it's an orphan (or the "lost+found" directory itself).  Note
 * "cur_ino" is the inode for the directory currently being
 * processed.
 *
 * Returns 1 if the name should NOT be obfuscated or 0 otherwise.
 */
static int
in_lost_found(
	xfs_ino_t		ino,
	int			namelen,
	unsigned char		*name)
{
	static xfs_ino_t	orphanage_ino = 0;
	char			s[24];	/* 21 is enough (64 bits in decimal) */
	int			slen;

	/* Record the "lost+found" inode if we haven't done so already */

	ASSERT(ino != 0);
	if (!orphanage_ino && is_orphanage_dir(mp, metadump.cur_ino, namelen,
						name))
		orphanage_ino = ino;

	/* We don't obfuscate the "lost+found" directory itself */

	if (ino == orphanage_ino)
		return 1;

	/* Most files aren't in "lost+found" at all */

	if (metadump.cur_ino != orphanage_ino)
		return 0;

	/*
	 * Within "lost+found", we don't obfuscate any file whose
	 * name is the same as its inode number.  Any others are
	 * stray files and can be obfuscated.
	 */
	slen = snprintf(s, sizeof (s), "%llu", (unsigned long long) ino);

	return slen == namelen && !memcmp(name, s, namelen);
}

/*
 * Look up the given name in the name table.  If it is already
 * present, iterate through a well-defined sequence of alternate
 * names and attempt to use an alternate name instead.
 *
 * Returns 1 if the (possibly modified) name is not present in the
 * name table.  Returns 0 if the name and all possible alternates
 * are already in the table.
 */
static int
handle_duplicate_name(xfs_dahash_t hash, size_t name_len, unsigned char *name)
{
	unsigned char	new_name[name_len + 1];
	uint32_t	seq = 1;

	if (!nametable_find(hash, name_len, name))
		return 1;	/* No duplicate */

	/* Name is already in use.  Need to find an alternate. */

	do {
		int	found;

		/* Only change incoming name if we find an alternate */
		do {
			memcpy(new_name, name, name_len);
			found = find_alternate(name_len, new_name, seq++);
			if (found < 0)
				return 0;	/* No more to check */
		} while (!found);
	} while (nametable_find(hash, name_len, new_name));

	/*
	 * The alternate wasn't in the table already.  Pass it back
	 * to the caller.
	 */
	memcpy(name, new_name, name_len);

	return 1;
}

static inline xfs_dahash_t
dirattr_hashname(
	bool		is_dirent,
	const uint8_t	*name,
	int		namelen)
{
	if (is_dirent) {
		struct xfs_name	xname = {
			.name	= name,
			.len	= namelen,
		};

		return libxfs_dir2_hashname(mp, &xname);
	}

	return libxfs_da_hashname(name, namelen);
}

static void
generate_obfuscated_name(
	xfs_ino_t		ino,
	int			namelen,
	unsigned char		*name)
{
	unsigned char		*orig_name = NULL;
	xfs_dahash_t		hash;

	/*
	 * We don't obfuscate "lost+found" or any orphan files
	 * therein.  When the name table is used for extended
	 * attributes, the inode number provided is 0, in which
	 * case we don't need to make this check.
	 */
	if (ino && in_lost_found(ino, namelen, name))
		return;

	/*
	 * If the name starts with a slash, just skip over it.  It
	 * isn't included in the hash and we don't record it in the
	 * name table.  Note that the namelen value passed in does
	 * not count the leading slash (if one is present).
	 */
	if (*name == '/')
		name++;

	/* Obfuscate the name (if possible) */
	hash = dirattr_hashname(ino != 0, name, namelen);

	/*
	 * If we're obfuscating a dirent name on a pptrs filesystem, see if we
	 * already processed the parent pointer and use the same name.
	 */
	if (xfs_has_parent(mp) && ino) {
		struct remap_ent	*remap;

		remap = remaptable_find(metadump.cur_ino, hash, name, namelen);
		if (remap) {
			remap_debug("found obfuscated dir 0x%lx '%.*s' -> 0x%lx -> '%.*s' \n",
					cur_ino, namelen,
					remap_ent_before(remap), ino, namelen,
					remap_ent_after(remap));
			memcpy(name, remap_ent_after(remap), namelen);
			return;
		}

		/*
		 * If we haven't procesed this dirent name before, save the
		 * old name for a remap table entry.  Obfuscate the name.
		 */
		orig_name = malloc(namelen);
		if (!orig_name) {
			orig_name = name;
			goto add_remap;
		}
		memcpy(orig_name, name, namelen);
	}

	obfuscate_name(hash, namelen, name, ino != 0);
	ASSERT(hash == dirattr_hashname(ino != 0, name, namelen));

	/*
	 * Make sure the name is not something already seen.  If we
	 * fail to find a suitable alternate, we're dealing with a
	 * very pathological situation, and we may end up creating
	 * a duplicate name in the metadump, so issue a warning.
	 */
	if (!handle_duplicate_name(hash, namelen, name)) {
		print_warning("duplicate name for inode %llu "
				"in dir inode %llu\n",
			(unsigned long long) ino,
			(unsigned long long) metadump.cur_ino);
		return;
	}

	/* Create an entry for the new name in the name table. */

	if (!nametable_add(hash, namelen, name))
		print_warning("unable to record name for inode %llu "
				"in dir inode %llu\n",
			(unsigned long long) ino,
			(unsigned long long) metadump.cur_ino);

	/*
	 * We've obfuscated a name in the directory entry.  Remember this
	 * remapping for when we come across the parent pointer later.
	 */
	if (!orig_name)
		return;

add_remap:
	remap_debug("obfuscating dir 0x%lx '%.*s' -> 0x%lx -> '%.*s' \n",
			metadump.cur_ino, namelen, orig_name, ino, namelen,
			name);

	if (!remaptable_add(metadump.cur_ino, hash, orig_name, namelen, name))
		print_warning("unable to record remapped dirent name for inode %llu "
				"in dir inode %llu\n",
			(unsigned long long) ino,
			(unsigned long long) metadump.cur_ino);
	if (orig_name && orig_name != name)
		free(orig_name);
}

static inline bool
want_obfuscate_dirents(bool is_meta)
{
	return metadump.obfuscate && !is_meta;
}

static void
process_sf_dir(
	struct xfs_dinode	*dip,
	bool			is_meta)
{
	struct xfs_dir2_sf_hdr	*sfp;
	xfs_dir2_sf_entry_t	*sfep;
	uint64_t		ino_dir_size;
	int			i;

	sfp = (struct xfs_dir2_sf_hdr *)XFS_DFORK_DPTR(dip);
	ino_dir_size = be64_to_cpu(dip->di_size);
	if (ino_dir_size > XFS_DFORK_DSIZE(dip, mp)) {
		ino_dir_size = XFS_DFORK_DSIZE(dip, mp);
		if (metadump.show_warnings)
			print_warning("invalid size in dir inode %llu",
					(long long)metadump.cur_ino);
	}

	sfep = xfs_dir2_sf_firstentry(sfp);
	for (i = 0; (i < sfp->count) &&
			((char *)sfep - (char *)sfp < ino_dir_size); i++) {

		/*
		 * first check for bad name lengths. If they are bad, we
		 * have limitations to how much can be obfuscated.
		 */
		int	namelen = sfep->namelen;

		if (namelen == 0) {
			if (metadump.show_warnings)
				print_warning("zero length entry in dir inode "
					"%llu", (long long)metadump.cur_ino);
			if (i != sfp->count - 1)
				break;
			namelen = ino_dir_size - ((char *)&sfep->name[0] -
					 (char *)sfp);
		} else if ((char *)sfep - (char *)sfp +
				libxfs_dir2_sf_entsize(mp, sfp, sfep->namelen) >
				ino_dir_size) {
			if (metadump.show_warnings)
				print_warning("entry length in dir inode %llu "
					"overflows space",
					(long long)metadump.cur_ino);
			if (i != sfp->count - 1)
				break;
			namelen = ino_dir_size - ((char *)&sfep->name[0] -
					 (char *)sfp);
		}

		if (want_obfuscate_dirents(is_meta))
			generate_obfuscated_name(
					 libxfs_dir2_sf_get_ino(mp, sfp, sfep),
					 namelen, &sfep->name[0]);

		sfep = (xfs_dir2_sf_entry_t *)((char *)sfep +
				libxfs_dir2_sf_entsize(mp, sfp, namelen));
	}

	/* zero stale data in rest of space in data fork, if any */
	if (metadump.zero_stale_data &&
	    (ino_dir_size < XFS_DFORK_DSIZE(dip, mp)))
		memset(sfep, 0, XFS_DFORK_DSIZE(dip, mp) - ino_dir_size);
}

/*
 * The pathname may not be null terminated. It may be terminated by the end of
 * a buffer or inode literal area, and the start of the next region contains
 * unknown data. Therefore, when we get to the last component of the symlink, we
 * cannot assume that strlen() will give us the right result. Hence we need to
 * track the remaining pathname length and use that instead.
 */
static void
obfuscate_path_components(
	char			*buf,
	uint64_t		len)
{
	unsigned char		*comp = (unsigned char *)buf;
	unsigned char		*end = comp + len;
	xfs_dahash_t		hash;

	while (comp < end) {
		char	*slash;
		int	namelen;

		/* find slash at end of this component */
		slash = strchr((char *)comp, '/');
		if (!slash) {
			/* last (or single) component */
			namelen = strnlen((char *)comp, len);
			hash = libxfs_da_hashname(comp, namelen);
			obfuscate_name(hash, namelen, comp, false);
			ASSERT(hash == libxfs_da_hashname(comp, namelen));
			break;
		}
		namelen = slash - (char *)comp;
		/* handle leading or consecutive slashes */
		if (!namelen) {
			comp++;
			len--;
			continue;
		}
		hash = libxfs_da_hashname(comp, namelen);
		obfuscate_name(hash, namelen, comp, false);
		ASSERT(hash == libxfs_da_hashname(comp, namelen));
		comp += namelen + 1;
		len -= namelen + 1;
	}
}

static void
process_sf_symlink(
	struct xfs_dinode	*dip)
{
	uint64_t		len;
	char			*buf;

	len = be64_to_cpu(dip->di_size);
	if (len > XFS_DFORK_DSIZE(dip, mp)) {
		if (metadump.show_warnings)
			print_warning("invalid size (%d) in symlink inode %llu",
					len, (long long)metadump.cur_ino);
		len = XFS_DFORK_DSIZE(dip, mp);
	}

	buf = (char *)XFS_DFORK_DPTR(dip);
	if (metadump.obfuscate)
		obfuscate_path_components(buf, len);

	/* zero stale data in rest of space in data fork, if any */
	if (metadump.zero_stale_data && len < XFS_DFORK_DSIZE(dip, mp))
		memset(&buf[len], 0, XFS_DFORK_DSIZE(dip, mp) - len);
}

static inline bool
want_obfuscate_pptr(
	unsigned int	nsp_flags,
	const void	*name,
	unsigned int	namelen,
	const void	*value,
	unsigned int	valuelen,
	bool		is_meta)
{
	if (!metadump.obfuscate || is_meta)
		return false;

	/* Ignore if parent pointers aren't enabled. */
	if (!xfs_has_parent(mp))
		return false;

	/* Ignore anything not claiming to be a parent pointer. */
	if (!(nsp_flags & XFS_ATTR_PARENT))
		return false;

	/* Obfuscate this parent pointer if it passes basic checks. */
	if (libxfs_parent_namecheck(mp, name, namelen, nsp_flags) &&
	    libxfs_parent_valuecheck(mp, value, valuelen) &&
	    libxfs_parent_hashcheck(mp, name, value, valuelen))
		return true;

	/* Ignore otherwise. */
	return false;
}

static void
obfuscate_parent_pointer(
	const struct xfs_parent_name_rec *rec,
	unsigned char			*value,
	unsigned int			valuelen)
{
	struct xfs_parent_name_irec	irec;
	struct remap_ent		*remap;
	char				*old_name = irec.p_name;
	xfs_dahash_t			hash;
	xfs_ino_t			child_ino = metadump.cur_ino;

	libxfs_parent_irec_from_disk(&irec, rec, value, valuelen);

	/*
	 * We don't obfuscate "lost+found" or any orphan files
	 * therein.  If When the name table is used for extended
	 * attributes, the inode number provided is 0, in which
	 * case we don't need to make this check.
	 */
	metadump.cur_ino = irec.p_ino;
	if (in_lost_found(child_ino, valuelen, value)) {
		metadump.cur_ino = child_ino;
		return;
	}
	metadump.cur_ino = child_ino;

	/*
	 * If the name starts with a slash, just skip over it.  It isn't
	 * included in the hash and we don't record it in the name table.
	 */
	if (*value == '/') {
		old_name++;
		value++;
		valuelen--;
	}

	hash = libxfs_da_hashname(value, valuelen);

	/*
	 * If we already processed the dirent, use the same name for the parent
	 * pointer.
	 */
	remap = remaptable_find(irec.p_ino, hash, value, valuelen);
	if (remap) {
		remap_debug("found obfuscated pptr 0x%lx '%.*s' -> 0x%lx -> '%.*s' \n",
				irec.p_ino, valuelen, remap_ent_before(remap),
				metadump.cur_ino, valuelen,
				remap_ent_after(remap));
		memcpy(value, remap_ent_after(remap), valuelen);
		return;
	}

	/*
	 * Obfuscate the parent pointer name and remember this for later
	 * in case we encounter the dirent and need to reuse the name there.
	 */
	obfuscate_name(hash, valuelen, value, true);

	remap_debug("obfuscated pptr 0x%lx '%.*s' -> 0x%lx -> '%.*s'\n",
			irec.p_ino, valuelen, old_name, metadump.cur_ino,
			valuelen, value);
	if (!remaptable_add(irec.p_ino, hash, old_name, valuelen, value))
		print_warning("unable to record remapped pptr name for inode %llu "
				"in dir inode %llu\n",
			(unsigned long long) metadump.cur_ino,
			(unsigned long long) irec.p_ino);
}

static inline bool
want_obfuscate_attr(
	unsigned int	nsp_flags,
	const void	*name,
	unsigned int	namelen,
	const void	*value,
	unsigned int	valuelen,
	bool		is_meta)
{
	if (!metadump.obfuscate || is_meta)
		return false;

	/*
	 * If we didn't already obfuscate the parent pointer, it's probably
	 * corrupt.  Leave it intact for analysis.
	 */
	if (nsp_flags & XFS_ATTR_PARENT)
		return false;

	return true;
}

static void
process_sf_attr(
	struct xfs_dinode		*dip,
	bool				is_meta)
{
	/*
	 * with extended attributes, obfuscate the names and fill the actual
	 * values with 'v' (to see a valid string length, as opposed to NULLs)
	 */

	struct xfs_attr_shortform	*asfp;
	struct xfs_attr_sf_entry	*asfep;
	int				ino_attr_size;
	int				i;

	asfp = (struct xfs_attr_shortform *)XFS_DFORK_APTR(dip);
	if (asfp->hdr.count == 0)
		return;

	ino_attr_size = be16_to_cpu(asfp->hdr.totsize);
	if (ino_attr_size > XFS_DFORK_ASIZE(dip, mp)) {
		ino_attr_size = XFS_DFORK_ASIZE(dip, mp);
		if (metadump.show_warnings)
			print_warning("invalid attr size in inode %llu",
					(long long)metadump.cur_ino);
	}

	asfep = &asfp->list[0];
	for (i = 0; (i < asfp->hdr.count) &&
			((char *)asfep - (char *)asfp < ino_attr_size); i++) {
		void	*name, *value;
		int	namelen = asfep->namelen;

		if (namelen == 0) {
			if (metadump.show_warnings)
				print_warning("zero length attr entry in inode "
					"%llu", (long long)metadump.cur_ino);
			break;
		} else if ((char *)asfep - (char *)asfp +
				xfs_attr_sf_entsize(asfep) > ino_attr_size) {
			if (metadump.show_warnings)
				print_warning("attr entry length in inode %llu "
					"overflows space",
					(long long)metadump.cur_ino);
			break;
		}

		name = &asfep->nameval[0];
		value = &asfep->nameval[asfep->namelen];

		if (want_obfuscate_pptr(asfep->flags, name, namelen,
					value, asfep->valuelen, is_meta)) {
			obfuscate_parent_pointer(name, value, asfep->valuelen);
		} else if (want_obfuscate_attr(asfep->flags, name, namelen,
					value, asfep->valuelen, is_meta)) {
			generate_obfuscated_name(0, asfep->namelen, name);
			memset(value, 'v', asfep->valuelen);
		}

		asfep = (struct xfs_attr_sf_entry *)((char *)asfep +
				xfs_attr_sf_entsize(asfep));
	}

	/* zero stale data in rest of space in attr fork, if any */
	if (metadump.zero_stale_data &&
	    (ino_attr_size < XFS_DFORK_ASIZE(dip, mp)))
		memset(asfep, 0, XFS_DFORK_ASIZE(dip, mp) - ino_attr_size);
}

static void
process_dir_free_block(
	char				*block)
{
	struct xfs_dir2_free		*free;
	struct xfs_dir3_icfree_hdr	freehdr;

	if (!metadump.zero_stale_data)
		return;

	free = (struct xfs_dir2_free *)block;
	libxfs_dir2_free_hdr_from_disk(mp, &freehdr, free);

	switch (freehdr.magic) {
	case XFS_DIR2_FREE_MAGIC:
	case XFS_DIR3_FREE_MAGIC: {
		__be16			*bests;
		char			*high;
		int			used;

		/* Zero out space from end of bests[] to end of block */
		bests = freehdr.bests;
		high = (char *)&bests[freehdr.nvalid];
		used = high - (char*)free;
		memset(high, 0, mp->m_dir_geo->blksize - used);
		iocur_top->need_crc = 1;
		break;
	}
	default:
		if (metadump.show_warnings)
			print_warning("invalid magic in dir inode %llu "
				      "free block",
				      (unsigned long long)metadump.cur_ino);
		break;
	}
}

static void
process_dir_leaf_block(
	char				*block)
{
	struct xfs_dir2_leaf		*leaf;
	struct xfs_dir3_icleaf_hdr	leafhdr;

	if (!metadump.zero_stale_data)
		return;

	/* Yes, this works for dir2 & dir3.  Difference is padding. */
	leaf = (struct xfs_dir2_leaf *)block;
	libxfs_dir2_leaf_hdr_from_disk(mp, &leafhdr, leaf);

	switch (leafhdr.magic) {
	case XFS_DIR2_LEAF1_MAGIC:
	case XFS_DIR3_LEAF1_MAGIC: {
		struct xfs_dir2_leaf_tail	*ltp;
		__be16				*lbp;
		char				*free; /* end of ents */

		/* Zero out space from end of ents[] to bests */
		free = (char *)&leafhdr.ents[leafhdr.count];
		ltp = xfs_dir2_leaf_tail_p(mp->m_dir_geo, leaf);
		lbp = xfs_dir2_leaf_bests_p(ltp);
		memset(free, 0, (char *)lbp - free);
		iocur_top->need_crc = 1;
		break;
	}
	case XFS_DIR2_LEAFN_MAGIC:
	case XFS_DIR3_LEAFN_MAGIC: {
		char				*free;
		int				used;

		/* Zero out space from end of ents[] to end of block */
		free = (char *)&leafhdr.ents[leafhdr.count];
		used = free - (char*)leaf;
		memset(free, 0, mp->m_dir_geo->blksize - used);
		iocur_top->need_crc = 1;
		break;
	}
	default:
		break;
	}
}

static void
process_dir_data_block(
	char		*block,
	xfs_fileoff_t	offset,
	int		is_block_format,
	bool		is_meta)
{
	/*
	 * we have to rely on the fileoffset and signature of the block to
	 * handle it's contents. If it's invalid, leave it alone.
	 * for multi-fsblock dir blocks, if a name crosses an extent boundary,
	 * ignore it and continue.
	 */
	int		dir_offset;
	char		*ptr;
	char		*endptr;
	int		end_of_data;
	int		wantmagic;
	struct xfs_dir2_data_hdr *datahdr;

	datahdr = (struct xfs_dir2_data_hdr *)block;

	if (is_block_format) {
		xfs_dir2_leaf_entry_t	*blp;
		xfs_dir2_block_tail_t	*btp;

		btp = xfs_dir2_block_tail_p(mp->m_dir_geo, datahdr);
		blp = xfs_dir2_block_leaf_p(btp);
		if ((char *)blp > (char *)btp)
			blp = (xfs_dir2_leaf_entry_t *)btp;

		end_of_data = (char *)blp - block;
		if (xfs_has_crc(mp))
			wantmagic = XFS_DIR3_BLOCK_MAGIC;
		else
			wantmagic = XFS_DIR2_BLOCK_MAGIC;
	} else { /* leaf/node format */
		end_of_data = mp->m_dir_geo->fsbcount << mp->m_sb.sb_blocklog;
		if (xfs_has_crc(mp))
			wantmagic = XFS_DIR3_DATA_MAGIC;
		else
			wantmagic = XFS_DIR2_DATA_MAGIC;
	}

	if (be32_to_cpu(datahdr->magic) != wantmagic) {
		if (metadump.show_warnings)
			print_warning(
		"invalid magic in dir inode %llu block %ld",
		(unsigned long long)metadump.cur_ino, (long)offset);
		return;
	}

	dir_offset = mp->m_dir_geo->data_entry_offset;
	ptr = block + dir_offset;
	endptr = block + mp->m_dir_geo->blksize;

	while (ptr < endptr && dir_offset < end_of_data) {
		xfs_dir2_data_entry_t	*dep;
		xfs_dir2_data_unused_t	*dup;
		int			length;

		dup = (xfs_dir2_data_unused_t *)ptr;

		if (be16_to_cpu(dup->freetag) == XFS_DIR2_DATA_FREE_TAG) {
			int	free_length = be16_to_cpu(dup->length);
			if (dir_offset + free_length > end_of_data ||
			    !free_length ||
			    (free_length & (XFS_DIR2_DATA_ALIGN - 1))) {
				if (metadump.show_warnings)
					print_warning(
			"invalid length for dir free space in inode %llu",
						(long long)metadump.cur_ino);
				return;
			}
			if (be16_to_cpu(*xfs_dir2_data_unused_tag_p(dup)) !=
					dir_offset)
				return;
			dir_offset += free_length;
			ptr += free_length;
			/*
			 * Zero the unused space up to the tag - the tag is
			 * actually at a variable offset, so zeroing &dup->tag
			 * is zeroing the free space in between
			 */
			if (metadump.zero_stale_data) {
				int zlen = free_length -
						sizeof(xfs_dir2_data_unused_t);

				if (zlen > 0) {
					memset(&dup->tag, 0, zlen);
					iocur_top->need_crc = 1;
				}
			}
			if (dir_offset >= end_of_data || ptr >= endptr)
				return;
		}

		dep = (xfs_dir2_data_entry_t *)ptr;
		length = libxfs_dir2_data_entsize(mp, dep->namelen);

		if (dir_offset + length > end_of_data ||
		    ptr + length > endptr) {
			if (metadump.show_warnings)
				print_warning(
			"invalid length for dir entry name in inode %llu",
					(long long)metadump.cur_ino);
			return;
		}
		if (be16_to_cpu(*libxfs_dir2_data_entry_tag_p(mp, dep)) !=
				dir_offset)
			return;

		if (want_obfuscate_dirents(is_meta))
			generate_obfuscated_name(be64_to_cpu(dep->inumber),
					 dep->namelen, &dep->name[0]);
		dir_offset += length;
		ptr += length;
		/* Zero the unused space after name, up to the tag */
		if (metadump.zero_stale_data) {
			/* 1 byte for ftype; don't bother with conditional */
			int zlen =
				(char *)libxfs_dir2_data_entry_tag_p(mp, dep) -
				(char *)&dep->name[dep->namelen] - 1;
			if (zlen > 0) {
				memset(&dep->name[dep->namelen] + 1, 0, zlen);
				iocur_top->need_crc = 1;
			}
		}
	}
}

static int
process_symlink_block(
	xfs_fileoff_t	o,
	xfs_fsblock_t	s,
	xfs_filblks_t	c,
	typnm_t		btype,
	xfs_fileoff_t	last,
	bool		is_meta)
{
	struct bbmap	map;
	char		*link;
	int		rval = 1;

	push_cur();
	map.nmaps = 1;
	map.b[0].bm_bn = XFS_FSB_TO_DADDR(mp, s);
	map.b[0].bm_len = XFS_FSB_TO_BB(mp, c);
	set_cur(&typtab[btype], 0, 0, DB_RING_IGN, &map);
	if (!iocur_top->data) {
		xfs_agnumber_t	agno = XFS_FSB_TO_AGNO(mp, s);
		xfs_agblock_t	agbno = XFS_FSB_TO_AGBNO(mp, s);

		print_warning("cannot read %s block %u/%u (%llu)",
				typtab[btype].name, agno, agbno, s);
		rval = !metadump.stop_on_read_error;
		goto out_pop;
	}
	link = iocur_top->data;

	if (xfs_has_crc((mp)))
		link += sizeof(struct xfs_dsymlink_hdr);

	if (want_obfuscate_dirents(is_meta))
		obfuscate_path_components(link, XFS_SYMLINK_BUF_SPACE(mp,
							mp->m_sb.sb_blocksize));
	if (metadump.zero_stale_data) {
		size_t	linklen, zlen;

		linklen = strlen(link);
		zlen = mp->m_sb.sb_blocksize - linklen;
		if (xfs_has_crc(mp))
			zlen -= sizeof(struct xfs_dsymlink_hdr);
		if (zlen < mp->m_sb.sb_blocksize)
			memset(link + linklen, 0, zlen);
	}

	iocur_top->need_crc = 1;
	if (write_buf(iocur_top))
		rval = 0;
out_pop:
	pop_cur();
	return rval;
}

#define MAX_REMOTE_VALS		4095

static struct attr_data_s {
	int			remote_val_count;
	xfs_dablk_t		remote_vals[MAX_REMOTE_VALS];
} attr_data;

static inline void
add_remote_vals(
	xfs_dablk_t 		blockidx,
	int			length)
{
	while (length > 0 && attr_data.remote_val_count < MAX_REMOTE_VALS) {
		attr_data.remote_vals[attr_data.remote_val_count] = blockidx;
		attr_data.remote_val_count++;
		blockidx++;
		length -= XFS_ATTR3_RMT_BUF_SPACE(mp, mp->m_sb.sb_blocksize);
	}

	if (attr_data.remote_val_count >= MAX_REMOTE_VALS) {
		print_warning(
"Overflowed attr obfuscation array. No longer obfuscating remote attrs.");
	}
}

/* Handle remote and leaf attributes */
static void
process_attr_block(
	char				*block,
	xfs_fileoff_t			offset,
	bool				is_meta)
{
	struct xfs_attr_leafblock	*leaf;
	struct xfs_attr3_icleaf_hdr	hdr;
	int				i;
	int				nentries;
	xfs_attr_leaf_entry_t 		*entry;
	xfs_attr_leaf_name_local_t 	*local;
	xfs_attr_leaf_name_remote_t 	*remote;
	uint32_t			bs = mp->m_sb.sb_blocksize;
	char				*first_name;


	leaf = (xfs_attr_leafblock_t *)block;

	/* Remote attributes - attr3 has XFS_ATTR3_RMT_MAGIC, attr has none */
	if ((be16_to_cpu(leaf->hdr.info.magic) != XFS_ATTR_LEAF_MAGIC) &&
	    (be16_to_cpu(leaf->hdr.info.magic) != XFS_ATTR3_LEAF_MAGIC)) {
		for (i = 0; i < attr_data.remote_val_count; i++) {
			if (metadump.obfuscate &&
			    attr_data.remote_vals[i] == offset)
				/* Macros to handle both attr and attr3 */
				memset(block +
					(bs - XFS_ATTR3_RMT_BUF_SPACE(mp, bs)),
				      'v', XFS_ATTR3_RMT_BUF_SPACE(mp, bs));
		}
		return;
	}

	/* Ok, it's a leaf - get header; accounts for crc & non-crc */
	libxfs_attr3_leaf_hdr_from_disk(mp->m_attr_geo, &hdr, leaf);

	nentries = hdr.count;
	if (nentries == 0 ||
	    nentries * sizeof(xfs_attr_leaf_entry_t) +
			xfs_attr3_leaf_hdr_size(leaf) >
				XFS_ATTR3_RMT_BUF_SPACE(mp, bs)) {
		if (metadump.show_warnings)
			print_warning("invalid attr count in inode %llu",
					(long long)metadump.cur_ino);
		return;
	}

	entry = xfs_attr3_leaf_entryp(leaf);
	/* We will move this as we parse */
	first_name = NULL;
	for (i = 0; i < nentries; i++, entry++) {
		int nlen, vlen, zlen;

		/* Grows up; if this name is topmost, move first_name */
		if (!first_name || xfs_attr3_leaf_name(leaf, i) < first_name)
			first_name = xfs_attr3_leaf_name(leaf, i);

		if (be16_to_cpu(entry->nameidx) > mp->m_sb.sb_blocksize) {
			if (metadump.show_warnings)
				print_warning(
				"invalid attr nameidx in inode %llu",
						(long long)metadump.cur_ino);
			break;
		}
		if (entry->flags & XFS_ATTR_LOCAL) {
			void *name, *value;
			unsigned int valuelen;

			local = xfs_attr3_leaf_name_local(leaf, i);
			if (local->namelen == 0) {
				if (metadump.show_warnings)
					print_warning(
				"zero length for attr name in inode %llu",
						(long long)metadump.cur_ino);
				break;
			}

			name = &local->nameval[0];
			value = &local->nameval[local->namelen];
			valuelen = be16_to_cpu(local->valuelen);

			if (want_obfuscate_pptr(entry->flags, name,
						local->namelen, value,
						valuelen, is_meta)) {
				obfuscate_parent_pointer(name, value, valuelen);
			} else if (want_obfuscate_attr(entry->flags, name,
						local->namelen, value,
						valuelen, is_meta)) {
				generate_obfuscated_name(0, local->namelen,
						name);
				memset(value, 'v', valuelen);
			}
			/* zero from end of nameval[] to next name start */
			nlen = local->namelen;
			vlen = be16_to_cpu(local->valuelen);
			zlen = xfs_attr_leaf_entsize_local(nlen, vlen) -
				(offsetof(struct xfs_attr_leaf_name_local, nameval) +
				 nlen + vlen);
			if (metadump.zero_stale_data)
				memset(&local->nameval[nlen + vlen], 0, zlen);
		} else {
			remote = xfs_attr3_leaf_name_remote(leaf, i);
			if (remote->namelen == 0 || remote->valueblk == 0) {
				if (metadump.show_warnings)
					print_warning(
				"invalid attr entry in inode %llu",
						(long long)metadump.cur_ino);
				break;
			}
			if (want_obfuscate_dirents(is_meta)) {
				generate_obfuscated_name(0, remote->namelen,
							 &remote->name[0]);
				add_remote_vals(be32_to_cpu(remote->valueblk),
						be32_to_cpu(remote->valuelen));
			}
			/* zero from end of name[] to next name start */
			nlen = remote->namelen;
			zlen = xfs_attr_leaf_entsize_remote(nlen) -
				(offsetof(struct xfs_attr_leaf_name_remote, name) +
				 nlen);
			if (metadump.zero_stale_data)
				memset(&remote->name[nlen], 0, zlen);
		}
	}

	/* Zero from end of entries array to the first name/val */
	if (metadump.zero_stale_data) {
		struct xfs_attr_leaf_entry *entries;

		entries = xfs_attr3_leaf_entryp(leaf);
		memset(&entries[nentries], 0,
		       first_name - (char *)&entries[nentries]);
	}
}

/* Processes symlinks, attrs, directories ... */
static int
process_single_fsb_objects(
	xfs_fileoff_t	o,
	xfs_fsblock_t	s,
	xfs_filblks_t	c,
	typnm_t		btype,
	xfs_fileoff_t	last,
	bool		is_meta)
{
	int		rval = 1;
	char		*dp;
	int		i;

	for (i = 0; i < c; i++) {
		push_cur();
		set_cur(&typtab[btype], XFS_FSB_TO_DADDR(mp, s), blkbb,
				DB_RING_IGN, NULL);

		if (!iocur_top->data) {
			xfs_agnumber_t	agno = XFS_FSB_TO_AGNO(mp, s);
			xfs_agblock_t	agbno = XFS_FSB_TO_AGBNO(mp, s);

			print_warning("cannot read %s block %u/%u (%llu)",
					typtab[btype].name, agno, agbno, s);
			rval = !metadump.stop_on_read_error;
			goto out_pop;

		}

		if (!metadump.obfuscate && !metadump.zero_stale_data)
			goto write;

		/* Zero unused part of interior nodes */
		if (metadump.zero_stale_data) {
			xfs_da_intnode_t *node = iocur_top->data;
			int magic = be16_to_cpu(node->hdr.info.magic);

			if (magic == XFS_DA_NODE_MAGIC ||
			    magic == XFS_DA3_NODE_MAGIC) {
				struct xfs_da3_icnode_hdr hdr;
				int used;

				libxfs_da3_node_hdr_from_disk(mp, &hdr, node);
				switch (btype) {
				case TYP_DIR2:
					used = mp->m_dir_geo->node_hdr_size;
					break;
				case TYP_ATTR:
					used = mp->m_attr_geo->node_hdr_size;
					break;
				default:
					/* unknown type, don't zero anything */
					used = mp->m_sb.sb_blocksize;
					break;
				}

				used += hdr.count
					* sizeof(struct xfs_da_node_entry);

				if (used < mp->m_sb.sb_blocksize) {
					memset((char *)node + used, 0,
						mp->m_sb.sb_blocksize - used);
					iocur_top->need_crc = 1;
				}
			}
		}

		/* Handle leaf nodes */
		dp = iocur_top->data;
		switch (btype) {
		case TYP_DIR2:
			if (o >= mp->m_dir_geo->freeblk) {
				process_dir_free_block(dp);
			} else if (o >= mp->m_dir_geo->leafblk) {
				process_dir_leaf_block(dp);
			} else {
				process_dir_data_block(dp, o,
					 last == mp->m_dir_geo->fsbcount,
					 is_meta);
			}
			iocur_top->need_crc = 1;
			break;
		case TYP_ATTR:
			process_attr_block(dp, o, is_meta);
			iocur_top->need_crc = 1;
			break;
		default:
			break;
		}

write:
		if (write_buf(iocur_top))
			rval = 0;
out_pop:
		pop_cur();
		if (!rval)
			break;
		o++;
		s++;
	}

	return rval;
}

/*
 * Static map to aggregate multiple extents into a single directory block.
 */
static struct bbmap mfsb_map;
static int mfsb_length;

static int
process_multi_fsb_dir(
	xfs_fileoff_t	o,
	xfs_fsblock_t	s,
	xfs_filblks_t	c,
	typnm_t		btype,
	xfs_fileoff_t	last,
	bool		is_meta)
{
	char		*dp;
	int		rval = 1;

	while (c > 0) {
		unsigned int	bm_len;

		if (mfsb_length + c >= mp->m_dir_geo->fsbcount) {
			bm_len = mp->m_dir_geo->fsbcount - mfsb_length;
			mfsb_length = 0;
		} else {
			mfsb_length += c;
			bm_len = c;
		}

		mfsb_map.b[mfsb_map.nmaps].bm_bn = XFS_FSB_TO_DADDR(mp, s);
		mfsb_map.b[mfsb_map.nmaps].bm_len = XFS_FSB_TO_BB(mp, bm_len);
		mfsb_map.nmaps++;

		if (mfsb_length == 0) {
			push_cur();
			set_cur(&typtab[btype], 0, 0, DB_RING_IGN, &mfsb_map);
			if (!iocur_top->data) {
				xfs_agnumber_t	agno = XFS_FSB_TO_AGNO(mp, s);
				xfs_agblock_t	agbno = XFS_FSB_TO_AGBNO(mp, s);

				print_warning("cannot read %s block %u/%u (%llu)",
						typtab[btype].name, agno, agbno, s);
				rval = !metadump.stop_on_read_error;
				goto out_pop;

			}

			if (!metadump.obfuscate && !metadump.zero_stale_data)
				goto write;

			dp = iocur_top->data;
			if (o >= mp->m_dir_geo->freeblk) {
				process_dir_free_block(dp);
			} else if (o >= mp->m_dir_geo->leafblk) {
				process_dir_leaf_block(dp);
			} else {
				process_dir_data_block(dp, o,
					 last == mp->m_dir_geo->fsbcount,
					 is_meta);
			}
			iocur_top->need_crc = 1;
write:
			if (write_buf(iocur_top))
				rval = 0;
out_pop:
			pop_cur();
			mfsb_map.nmaps = 0;
			if (!rval)
				break;
		}
		c -= bm_len;
		s += bm_len;
	}

	return rval;
}

static bool
is_multi_fsb_object(
	struct xfs_mount	*mp,
	typnm_t			btype)
{
	if (btype == TYP_DIR2 && mp->m_dir_geo->fsbcount > 1)
		return true;
	if (btype == TYP_SYMLINK)
		return true;
	return false;
}

static int
process_multi_fsb_objects(
	xfs_fileoff_t	o,
	xfs_fsblock_t	s,
	xfs_filblks_t	c,
	typnm_t		btype,
	xfs_fileoff_t	last,
	bool		is_meta)
{
	switch (btype) {
	case TYP_DIR2:
		return process_multi_fsb_dir(o, s, c, btype, last, is_meta);
	case TYP_SYMLINK:
		return process_symlink_block(o, s, c, btype, last, is_meta);
	default:
		print_warning("bad type for multi-fsb object %d", btype);
		return 1;
	}
}

/* inode copy routines */
static int
process_bmbt_reclist(
	xfs_bmbt_rec_t		*rp,
	int			numrecs,
	typnm_t			btype,
	bool			is_meta)
{
	int			i;
	xfs_fileoff_t		o, op = NULLFILEOFF;
	xfs_fsblock_t		s;
	xfs_filblks_t		c, cp = NULLFILEOFF;
	int			f;
	xfs_fileoff_t		last;
	xfs_agnumber_t		agno;
	xfs_agblock_t		agbno;
	bool			is_multi_fsb = is_multi_fsb_object(mp, btype);
	int			rval = 1;

	if (btype == TYP_DATA)
		return 1;

	convert_extent(&rp[numrecs - 1], &o, &s, &c, &f);
	last = o + c;

	for (i = 0; i < numrecs; i++, rp++) {
		convert_extent(rp, &o, &s, &c, &f);

		/*
		 * ignore extents that are clearly bogus, and if a bogus
		 * one is found, stop processing remaining extents
		 */
		if (i > 0 && op + cp > o) {
			if (metadump.show_warnings)
				print_warning("bmap extent %d in %s ino %llu "
					"starts at %llu, previous extent "
					"ended at %llu", i,
					typtab[btype].name,
					(long long)metadump.cur_ino,
					o, op + cp - 1);
			break;
		}

		if (c > metadump.max_extent_size) {
			/*
			 * since we are only processing non-data extents,
			 * large numbers of blocks in a metadata extent is
			 * extremely rare and more than likely to be corrupt.
			 */
			if (metadump.show_warnings)
				print_warning("suspicious count %u in bmap "
					"extent %d in %s ino %llu", c, i,
					typtab[btype].name,
					(long long)metadump.cur_ino);
			break;
		}

		op = o;
		cp = c;

		agno = XFS_FSB_TO_AGNO(mp, s);
		agbno = XFS_FSB_TO_AGBNO(mp, s);

		if (!valid_bno(agno, agbno)) {
			if (metadump.show_warnings)
				print_warning("invalid block number %u/%u "
					"(%llu) in bmap extent %d in %s ino "
					"%llu", agno, agbno, s, i,
					typtab[btype].name,
					(long long)metadump.cur_ino);
			break;
		}

		if (!valid_bno(agno, agbno + c - 1)) {
			if (metadump.show_warnings)
				print_warning("bmap extent %i in %s inode %llu "
					"overflows AG (end is %u/%u)", i,
					typtab[btype].name,
					(long long)metadump.cur_ino,
					agno, agbno + c - 1);
			break;
		}

		/* multi-extent blocks require special handling */
		if (is_multi_fsb)
			rval = process_multi_fsb_objects(o, s, c, btype,
					last, is_meta);
		else
			rval = process_single_fsb_objects(o, s, c, btype,
					last, is_meta);
		if (!rval)
			break;
	}

	return rval;
}

struct scan_bmap {
	enum typnm	typ;
	bool		is_meta;
};

static int
scanfunc_bmap(
	struct xfs_btree_block	*block,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	int			level,
	typnm_t			btype,
	void			*arg)	/* ptr to itype */
{
	struct scan_bmap	*sbm = arg;
	int			i;
	xfs_bmbt_ptr_t		*pp;
	int			nrecs;

	nrecs = be16_to_cpu(block->bb_numrecs);

	if (level == 0) {
		if (nrecs > mp->m_bmap_dmxr[0]) {
			if (metadump.show_warnings)
				print_warning("invalid numrecs (%u) in %s "
					"block %u/%u", nrecs,
					typtab[btype].name, agno, agbno);
			return 1;
		}
		return process_bmbt_reclist(XFS_BMBT_REC_ADDR(mp, block, 1),
					    nrecs, sbm->typ, sbm->is_meta);
	}

	if (nrecs > mp->m_bmap_dmxr[1]) {
		if (metadump.show_warnings)
			print_warning("invalid numrecs (%u) in %s block %u/%u",
					nrecs, typtab[btype].name, agno, agbno);
		return 1;
	}
	pp = XFS_BMBT_PTR_ADDR(mp, block, 1, mp->m_bmap_dmxr[1]);
	for (i = 0; i < nrecs; i++) {
		xfs_agnumber_t	ag;
		xfs_agblock_t	bno;

		ag = XFS_FSB_TO_AGNO(mp, get_unaligned_be64(&pp[i]));
		bno = XFS_FSB_TO_AGBNO(mp, get_unaligned_be64(&pp[i]));

		if (bno == 0 || bno > mp->m_sb.sb_agblocks ||
				ag > mp->m_sb.sb_agcount) {
			if (metadump.show_warnings)
				print_warning("invalid block number (%u/%u) "
					"in %s block %u/%u", ag, bno,
					typtab[btype].name, agno, agbno);
			continue;
		}

		if (!scan_btree(ag, bno, level, btype, arg, scanfunc_bmap))
			return 0;
	}
	return 1;
}

static inline bool
is_metadata_ino(
	struct xfs_dinode	*dip)
{
	return xfs_has_metadir(mp) &&
			dip->di_version >= 3 &&
			(dip->di_flags2 & cpu_to_be64(XFS_DIFLAG2_METADIR));
}

static int
process_btinode(
	struct xfs_dinode 	*dip,
	typnm_t			itype)
{
	xfs_bmdr_block_t	*dib;
	int			i;
	xfs_bmbt_ptr_t		*pp;
	int			level;
	int			nrecs;
	int			maxrecs;
	int			whichfork;
	typnm_t			btype;
	bool			is_meta = is_metadata_ino(dip);

	whichfork = (itype == TYP_ATTR) ? XFS_ATTR_FORK : XFS_DATA_FORK;
	btype = (itype == TYP_ATTR) ? TYP_BMAPBTA : TYP_BMAPBTD;

	dib = (xfs_bmdr_block_t *)XFS_DFORK_PTR(dip, whichfork);
	level = be16_to_cpu(dib->bb_level);
	nrecs = be16_to_cpu(dib->bb_numrecs);

	if (level > XFS_BM_MAXLEVELS(mp, whichfork)) {
		if (metadump.show_warnings)
			print_warning("invalid level (%u) in inode %lld %s "
				"root", level, (long long)metadump.cur_ino,
				typtab[btype].name);
		return 1;
	}

	if (level == 0) {
		return process_bmbt_reclist(XFS_BMDR_REC_ADDR(dib, 1),
					    nrecs, itype, is_meta);
	}

	maxrecs = libxfs_bmdr_maxrecs(XFS_DFORK_SIZE(dip, mp, whichfork), 0);
	if (nrecs > maxrecs) {
		if (metadump.show_warnings)
			print_warning("invalid numrecs (%u) in inode %lld %s "
				"root", nrecs, (long long)metadump.cur_ino,
				typtab[btype].name);
		return 1;
	}

	pp = XFS_BMDR_PTR_ADDR(dib, 1, maxrecs);

	if (metadump.zero_stale_data) {
		char	*top;

		/* Unused btree key space */
		top = (char*)XFS_BMDR_KEY_ADDR(dib, nrecs + 1);
		memset(top, 0, (char*)pp - top);

		/* Unused btree ptr space */
		top = (char*)&pp[nrecs];
		memset(top, 0, (char*)dib + XFS_DFORK_SIZE(dip, mp, whichfork) - top);
	}

	for (i = 0; i < nrecs; i++) {
		struct scan_bmap	sbm = {
			.typ = itype,
			.is_meta = is_meta,
		};
		xfs_agnumber_t	ag;
		xfs_agblock_t	bno;

		ag = XFS_FSB_TO_AGNO(mp, get_unaligned_be64(&pp[i]));
		bno = XFS_FSB_TO_AGBNO(mp, get_unaligned_be64(&pp[i]));

		if (bno == 0 || bno > mp->m_sb.sb_agblocks ||
				ag > mp->m_sb.sb_agcount) {
			if (metadump.show_warnings)
				print_warning("invalid block number (%u/%u) "
					"in inode %llu %s root", ag, bno,
					(long long)metadump.cur_ino,
					typtab[btype].name);
			continue;
		}

		if (!scan_btree(ag, bno, level, btype, &sbm, scanfunc_bmap))
			return 0;
	}
	return 1;
}

static int
process_exinode(
	struct xfs_dinode 	*dip,
	typnm_t			itype)
{
	int			whichfork;
	int			used;
	xfs_extnum_t		nex, max_nex;
	bool			is_meta = is_metadata_ino(dip);

	whichfork = (itype == TYP_ATTR) ? XFS_ATTR_FORK : XFS_DATA_FORK;

	nex = xfs_dfork_nextents(dip, whichfork);
	max_nex = xfs_iext_max_nextents(
			xfs_dinode_has_large_extent_counts(dip),
			whichfork);
	used = nex * sizeof(xfs_bmbt_rec_t);
	if (nex > max_nex || used > XFS_DFORK_SIZE(dip, mp, whichfork)) {
		if (metadump.show_warnings)
			print_warning("bad number of extents %llu in inode %lld",
				(unsigned long long)nex,
				(long long)metadump.cur_ino);
		return 1;
	}

	/* Zero unused data fork past used extents */
	if (metadump.zero_stale_data &&
	    (used < XFS_DFORK_SIZE(dip, mp, whichfork)))
		memset(XFS_DFORK_PTR(dip, whichfork) + used, 0,
		       XFS_DFORK_SIZE(dip, mp, whichfork) - used);


	return process_bmbt_reclist((xfs_bmbt_rec_t *)XFS_DFORK_PTR(dip,
					whichfork), nex, itype, is_meta);
}

static int
process_inode_data(
	struct xfs_dinode	*dip,
	typnm_t			itype)
{
	bool			is_meta = is_metadata_ino(dip);

	switch (dip->di_format) {
		case XFS_DINODE_FMT_LOCAL:
			if (!(metadump.obfuscate || metadump.zero_stale_data))
				break;

			/*
			 * If the fork size is invalid, we can't safely do
			 * anything with this fork. Leave it alone to preserve
			 * the information for diagnostic purposes.
			 */
			if (XFS_DFORK_DSIZE(dip, mp) > XFS_LITINO(mp)) {
				print_warning(
"Invalid data fork size (%d) in inode %llu, preserving contents!",
						XFS_DFORK_DSIZE(dip, mp),
						(long long)metadump.cur_ino);
				break;
			}

			switch (itype) {
				case TYP_DIR2:
					process_sf_dir(dip, is_meta);
					break;

				case TYP_SYMLINK:
					process_sf_symlink(dip);
					break;

				default:
					break;
			}
			break;

		case XFS_DINODE_FMT_EXTENTS:
			return process_exinode(dip, itype);

		case XFS_DINODE_FMT_BTREE:
			return process_btinode(dip, itype);
	}
	return 1;
}

static void
process_dev_inode(
	struct xfs_dinode		*dip)
{
	if (xfs_dfork_data_extents(dip)) {
		if (metadump.show_warnings)
			print_warning("inode %llu has unexpected extents",
				      (unsigned long long)metadump.cur_ino);
		return;
	}

	/*
	 * If the fork size is invalid, we can't safely do anything with
	 * this fork. Leave it alone to preserve the information for diagnostic
	 * purposes.
	 */
	if (XFS_DFORK_DSIZE(dip, mp) > XFS_LITINO(mp)) {
		print_warning(
"Invalid data fork size (%d) in inode %llu, preserving contents!",
			XFS_DFORK_DSIZE(dip, mp), (long long)metadump.cur_ino);
		return;
	}

	if (metadump.zero_stale_data) {
		unsigned int	size = sizeof(xfs_dev_t);

		memset(XFS_DFORK_DPTR(dip) + size, 0,
				XFS_DFORK_DSIZE(dip, mp) - size);
	}
}

/*
 * when we process the inode, we may change the data in the data and/or
 * attribute fork if they are in short form and we are obfuscating names.
 * In this case we need to recalculate the CRC of the inode, but we should
 * only do that if the CRC in the inode is good to begin with. If the crc
 * is not ok, we just leave it alone.
 */
static int
process_inode(
	xfs_agnumber_t		agno,
	xfs_agino_t 		agino,
	struct xfs_dinode 	*dip,
	bool			free_inode)
{
	int			rval = 1;
	bool			crc_was_ok = false; /* no recalc by default */
	bool			need_new_crc = false;

	metadump.cur_ino = XFS_AGINO_TO_INO(mp, agno, agino);

	/* we only care about crc recalculation if we will modify the inode. */
	if (metadump.obfuscate || metadump.zero_stale_data) {
		crc_was_ok = libxfs_verify_cksum((char *)dip,
					mp->m_sb.sb_inodesize,
					offsetof(struct xfs_dinode, di_crc));
	}

	if (free_inode) {
		if (metadump.zero_stale_data) {
			/* Zero all of the inode literal area */
			memset(XFS_DFORK_DPTR(dip), 0, XFS_LITINO(mp));
		}
		goto done;
	}

	/* copy appropriate data fork metadata */
	switch (be16_to_cpu(dip->di_mode) & S_IFMT) {
		case S_IFDIR:
			rval = process_inode_data(dip, TYP_DIR2);
			if (dip->di_format == XFS_DINODE_FMT_LOCAL)
				need_new_crc = true;
			break;
		case S_IFLNK:
			rval = process_inode_data(dip, TYP_SYMLINK);
			if (dip->di_format == XFS_DINODE_FMT_LOCAL)
				need_new_crc = true;
			break;
		case S_IFREG:
			rval = process_inode_data(dip, TYP_DATA);
			break;
		case S_IFIFO:
		case S_IFCHR:
		case S_IFBLK:
		case S_IFSOCK:
			process_dev_inode(dip);
			need_new_crc = true;
			break;
		default:
			break;
	}
	nametable_clear();
	if (!rval)
		goto done;

	/* copy extended attributes if they exist and forkoff is valid */
	if (XFS_DFORK_DSIZE(dip, mp) < XFS_LITINO(mp)) {
		bool	is_meta = is_metadata_ino(dip);

		attr_data.remote_val_count = 0;
		switch (dip->di_aformat) {
			case XFS_DINODE_FMT_LOCAL:
				need_new_crc = true;
				if (metadump.obfuscate ||
				    metadump.zero_stale_data)
					process_sf_attr(dip, is_meta);
				break;

			case XFS_DINODE_FMT_EXTENTS:
				rval = process_exinode(dip, TYP_ATTR);
				break;

			case XFS_DINODE_FMT_BTREE:
				rval = process_btinode(dip, TYP_ATTR);
				break;
		}
		nametable_clear();
	}

done:
	/* Heavy handed but low cost; just do it as a catch-all. */
	if (metadump.zero_stale_data)
		need_new_crc = true;

	if (crc_was_ok && need_new_crc)
		libxfs_dinode_calc_crc(mp, dip);

	return rval;
}

static uint32_t	inodes_copied;

static int
copy_inode_chunk(
	xfs_agnumber_t 		agno,
	xfs_inobt_rec_t 	*rp)
{
	xfs_agino_t 		agino;
	int			off;
	xfs_agblock_t		agbno;
	xfs_agblock_t		end_agbno;
	int			i;
	int			rval = 0;
	int			blks_per_buf;
	int			inodes_per_buf;
	int			ioff;
	struct xfs_ino_geometry *igeo = M_IGEO(mp);

	agino = be32_to_cpu(rp->ir_startino);
	agbno = XFS_AGINO_TO_AGBNO(mp, agino);
	end_agbno = agbno + igeo->ialloc_blks;
	off = XFS_INO_TO_OFFSET(mp, agino);

	/*
	 * If the fs supports sparse inode records, we must process inodes a
	 * cluster at a time because that is the sparse allocation granularity.
	 * Otherwise, we risk CRC corruption errors on reads of inode chunks.
	 *
	 * Also make sure that that we don't process more than the single record
	 * we've been passed (large block sizes can hold multiple inode chunks).
	 */
	if (xfs_has_sparseinodes(mp))
		blks_per_buf = igeo->blocks_per_cluster;
	else
		blks_per_buf = igeo->ialloc_blks;
	inodes_per_buf = min(XFS_FSB_TO_INO(mp, blks_per_buf),
			     XFS_INODES_PER_CHUNK);

	/*
	 * Sanity check that we only process a single buffer if ir_startino has
	 * a buffer offset. A non-zero offset implies that the entire chunk lies
	 * within a block.
	 */
	if (off && inodes_per_buf != XFS_INODES_PER_CHUNK) {
		print_warning("bad starting inode offset %d", off);
		return 0;
	}

	if (agino == 0 || agino == NULLAGINO || !valid_bno(agno, agbno) ||
			!valid_bno(agno, XFS_AGINO_TO_AGBNO(mp,
					agino + XFS_INODES_PER_CHUNK - 1))) {
		if (metadump.show_warnings)
			print_warning("bad inode number %llu (%u/%u)",
				XFS_AGINO_TO_INO(mp, agno, agino), agno, agino);
		return 1;
	}

	/*
	 * check for basic assumptions about inode chunks, and if any
	 * assumptions fail, don't process the inode chunk.
	 */
	if ((mp->m_sb.sb_inopblock <= XFS_INODES_PER_CHUNK && off != 0) ||
			(mp->m_sb.sb_inopblock > XFS_INODES_PER_CHUNK &&
					off % XFS_INODES_PER_CHUNK != 0) ||
			(xfs_has_align(mp) &&
					mp->m_sb.sb_inoalignmt != 0 &&
					agbno % mp->m_sb.sb_inoalignmt != 0)) {
		if (metadump.show_warnings)
			print_warning("badly aligned inode (start = %llu)",
					XFS_AGINO_TO_INO(mp, agno, agino));
		return 1;
	}

	push_cur();
	ioff = 0;
	while (agbno < end_agbno && ioff < XFS_INODES_PER_CHUNK) {
		if (xfs_inobt_is_sparse_disk(rp, ioff))
			goto next_bp;

		set_cur(&typtab[TYP_INODE], XFS_AGB_TO_DADDR(mp, agno, agbno),
			XFS_FSB_TO_BB(mp, blks_per_buf), DB_RING_IGN, NULL);
		if (iocur_top->data == NULL) {
			print_warning("cannot read inode block %u/%u",
				      agno, agbno);
			rval = !metadump.stop_on_read_error;
			goto pop_out;
		}

		for (i = 0; i < inodes_per_buf; i++) {
			struct xfs_dinode	*dip;

			dip = (struct xfs_dinode *)((char *)iocur_top->data +
					((off + i) << mp->m_sb.sb_inodelog));

			/* process_inode handles free inodes, too */
			if (!process_inode(agno, agino + ioff + i, dip,
					XFS_INOBT_IS_FREE_DISK(rp, ioff + i)))
				goto pop_out;

			inodes_copied++;
		}

		if (write_buf(iocur_top))
			goto pop_out;

next_bp:
		agbno += blks_per_buf;
		ioff += inodes_per_buf;
	}

	if (metadump.show_progress)
		print_progress("Copied %u of %u inodes (%u of %u AGs)",
				inodes_copied, mp->m_sb.sb_icount, agno,
				mp->m_sb.sb_agcount);
	rval = 1;
pop_out:
	pop_cur();
	return rval;
}

static int
scanfunc_ino(
	struct xfs_btree_block	*block,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	int			level,
	typnm_t			btype,
	void			*arg)
{
	xfs_inobt_rec_t		*rp;
	xfs_inobt_ptr_t		*pp;
	int			i;
	int			numrecs;
	int			finobt = *(int *) arg;
	struct xfs_ino_geometry	*igeo = M_IGEO(mp);

	numrecs = be16_to_cpu(block->bb_numrecs);

	if (level == 0) {
		if (numrecs > igeo->inobt_mxr[0]) {
			if (metadump.show_warnings)
				print_warning("invalid numrecs %d in %s "
					"block %u/%u", numrecs,
					typtab[btype].name, agno, agbno);
			numrecs = igeo->inobt_mxr[0];
		}

		/*
		 * Only copy the btree blocks for the finobt. The inobt scan
		 * copies the inode chunks.
		 */
		if (finobt)
			return 1;

		rp = XFS_INOBT_REC_ADDR(mp, block, 1);
		for (i = 0; i < numrecs; i++, rp++) {
			if (!copy_inode_chunk(agno, rp))
				return 0;
		}
		return 1;
	}

	if (numrecs > igeo->inobt_mxr[1]) {
		if (metadump.show_warnings)
			print_warning("invalid numrecs %d in %s block %u/%u",
				numrecs, typtab[btype].name, agno, agbno);
		numrecs = igeo->inobt_mxr[1];
	}

	pp = XFS_INOBT_PTR_ADDR(mp, block, 1, igeo->inobt_mxr[1]);
	for (i = 0; i < numrecs; i++) {
		if (!valid_bno(agno, be32_to_cpu(pp[i]))) {
			if (metadump.show_warnings)
				print_warning("invalid block number (%u/%u) "
					"in %s block %u/%u",
					agno, be32_to_cpu(pp[i]),
					typtab[btype].name, agno, agbno);
			continue;
		}
		if (!scan_btree(agno, be32_to_cpu(pp[i]), level,
				btype, arg, scanfunc_ino))
			return 0;
	}
	return 1;
}

static int
copy_inodes(
	xfs_agnumber_t		agno,
	xfs_agi_t		*agi)
{
	xfs_agblock_t		root;
	int			levels;
	int			finobt = 0;

	root = be32_to_cpu(agi->agi_root);
	levels = be32_to_cpu(agi->agi_level);

	/* validate root and levels before processing the tree */
	if (root == 0 || root > mp->m_sb.sb_agblocks) {
		if (metadump.show_warnings)
			print_warning("invalid block number (%u) in inobt "
					"root in agi %u", root, agno);
		return 1;
	}
	if (levels > M_IGEO(mp)->inobt_maxlevels) {
		if (metadump.show_warnings)
			print_warning("invalid level (%u) in inobt root "
					"in agi %u", levels, agno);
		return 1;
	}

	if (!scan_btree(agno, root, levels, TYP_INOBT, &finobt, scanfunc_ino))
		return 0;

	if (xfs_has_finobt(mp)) {
		root = be32_to_cpu(agi->agi_free_root);
		levels = be32_to_cpu(agi->agi_free_level);

		if (root == 0 || root > mp->m_sb.sb_agblocks) {
			if (metadump.show_warnings)
				print_warning("invalid block number (%u) in "
						"finobt root in agi %u", root,
						agno);
			return 1;
		}

		if (levels > M_IGEO(mp)->inobt_maxlevels) {
			if (metadump.show_warnings)
				print_warning("invalid level (%u) in finobt "
						"root in agi %u", levels, agno);
			return 1;
		}

		finobt = 1;
		if (!scan_btree(agno, root, levels, TYP_FINOBT, &finobt,
				scanfunc_ino))
			return 0;
	}

	return 1;
}

static int
scan_ag(
	xfs_agnumber_t	agno)
{
	xfs_agf_t	*agf;
	xfs_agi_t	*agi;
	int		stack_count = 0;
	int		rval = 0;

	/* copy the superblock of the AG */
	push_cur();
	stack_count++;
	set_cur(&typtab[TYP_SB], XFS_AG_DADDR(mp, agno, XFS_SB_DADDR),
			XFS_FSS_TO_BB(mp, 1), DB_RING_IGN, NULL);
	if (!iocur_top->data) {
		print_warning("cannot read superblock for ag %u", agno);
		if (metadump.stop_on_read_error)
			goto pop_out;
	} else {
		/* Replace any filesystem label with "L's" */
		if (metadump.obfuscate) {
			struct xfs_sb *sb = iocur_top->data;
			memset(sb->sb_fname, 'L',
			       min(strlen(sb->sb_fname), sizeof(sb->sb_fname)));
			iocur_top->need_crc = 1;
		}
		if (write_buf(iocur_top))
			goto pop_out;
	}

	/* copy the AG free space btree root */
	push_cur();
	stack_count++;
	set_cur(&typtab[TYP_AGF], XFS_AG_DADDR(mp, agno, XFS_AGF_DADDR(mp)),
			XFS_FSS_TO_BB(mp, 1), DB_RING_IGN, NULL);
	agf = iocur_top->data;
	if (iocur_top->data == NULL) {
		print_warning("cannot read agf block for ag %u", agno);
		if (metadump.stop_on_read_error)
			goto pop_out;
	} else {
		if (write_buf(iocur_top))
			goto pop_out;
	}

	/* copy the AG inode btree root */
	push_cur();
	stack_count++;
	set_cur(&typtab[TYP_AGI], XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)),
			XFS_FSS_TO_BB(mp, 1), DB_RING_IGN, NULL);
	agi = iocur_top->data;
	if (iocur_top->data == NULL) {
		print_warning("cannot read agi block for ag %u", agno);
		if (metadump.stop_on_read_error)
			goto pop_out;
	} else {
		if (write_buf(iocur_top))
			goto pop_out;
	}

	/* copy the AG free list header */
	push_cur();
	stack_count++;
	set_cur(&typtab[TYP_AGFL], XFS_AG_DADDR(mp, agno, XFS_AGFL_DADDR(mp)),
			XFS_FSS_TO_BB(mp, 1), DB_RING_IGN, NULL);
	if (iocur_top->data == NULL) {
		print_warning("cannot read agfl block for ag %u", agno);
		if (metadump.stop_on_read_error)
			goto pop_out;
	} else {
		if (agf && metadump.zero_stale_data) {
			/* Zero out unused bits of agfl */
			int i;
			 __be32  *agfl_bno;

			agfl_bno = xfs_buf_to_agfl_bno(iocur_top->bp);
			i = be32_to_cpu(agf->agf_fllast);

			for (;;) {
				if (++i == libxfs_agfl_size(mp))
					i = 0;
				if (i == be32_to_cpu(agf->agf_flfirst))
					break;
				agfl_bno[i] = cpu_to_be32(NULLAGBLOCK);
			}
			iocur_top->need_crc = 1;
		}
		if (write_buf(iocur_top))
			goto pop_out;
	}

	/* copy AG free space btrees */
	if (agf) {
		if (metadump.show_progress)
			print_progress("Copying free space trees of AG %u",
					agno);
		if (!copy_free_bno_btree(agno, agf))
			goto pop_out;
		if (!copy_free_cnt_btree(agno, agf))
			goto pop_out;
		if (!copy_rmap_btree(agno, agf))
			goto pop_out;
		if (!copy_refcount_btree(agno, agf))
			goto pop_out;
	}

	/* copy inode btrees and the inodes and their associated metadata */
	if (agi) {
		if (!copy_inodes(agno, agi))
			goto pop_out;
	}
	rval = 1;
pop_out:
	while (stack_count--)
		pop_cur();
	return rval;
}

static int
copy_ino(
	xfs_ino_t		ino,
	typnm_t			itype)
{
	xfs_agnumber_t		agno;
	xfs_agblock_t		agbno;
	xfs_agino_t		agino;
	int			offset;
	int			rval = 1;

	if (ino == 0 || ino == NULLFSINO)
		return 1;

	agno = XFS_INO_TO_AGNO(mp, ino);
	agino = XFS_INO_TO_AGINO(mp, ino);
	agbno = XFS_AGINO_TO_AGBNO(mp, agino);
	offset = XFS_AGINO_TO_OFFSET(mp, agino);

	if (agno >= mp->m_sb.sb_agcount || agbno >= mp->m_sb.sb_agblocks ||
			offset >= mp->m_sb.sb_inopblock) {
		if (metadump.show_warnings)
			print_warning("invalid %s inode number (%lld)",
					typtab[itype].name, (long long)ino);
		return 1;
	}

	push_cur();
	set_cur(&typtab[TYP_INODE], XFS_AGB_TO_DADDR(mp, agno, agbno),
			blkbb, DB_RING_IGN, NULL);
	if (iocur_top->data == NULL) {
		print_warning("cannot read %s inode %lld",
				typtab[itype].name, (long long)ino);
		rval = !metadump.stop_on_read_error;
		goto pop_out;
	}
	off_cur(offset << mp->m_sb.sb_inodelog, mp->m_sb.sb_inodesize);

	metadump.cur_ino = ino;
	rval = process_inode_data(iocur_top->data, itype);
pop_out:
	pop_cur();
	return rval;
}


static int
copy_sb_inodes(void)
{
	if (!copy_ino(mp->m_sb.sb_rbmino, TYP_RTBITMAP))
		return 0;

	if (!copy_ino(mp->m_sb.sb_rsumino, TYP_RTSUMMARY))
		return 0;

	if (!copy_ino(mp->m_sb.sb_uquotino, TYP_DQBLK))
		return 0;

	if (!copy_ino(mp->m_sb.sb_gquotino, TYP_DQBLK))
		return 0;

	return copy_ino(mp->m_sb.sb_pquotino, TYP_DQBLK);
}

static int
copy_log(void)
{
	struct xlog	log;
	int		dirty;
	xfs_daddr_t	logstart;
	int		logblocks;
	int		logversion;
	int		cycle = XLOG_INIT_CYCLE;

	if (metadump.show_progress)
		print_progress("Copying log");

	push_cur();
	if (metadump.external_log) {
		ASSERT(mp->m_sb.sb_logstart == 0);
		set_log_cur(&typtab[TYP_LOG],
				XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart),
				mp->m_sb.sb_logblocks * blkbb, DB_RING_IGN,
				NULL);
	} else {
		ASSERT(mp->m_sb.sb_logstart != 0);
		set_cur(&typtab[TYP_LOG],
				XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart),
				mp->m_sb.sb_logblocks * blkbb, DB_RING_IGN,
				NULL);
	}

	if (iocur_top->data == NULL) {
		pop_cur();
		print_warning("cannot read log");
		return !metadump.stop_on_read_error;
	}

	/* If not obfuscating or zeroing, just copy the log as it is */
	if (!metadump.obfuscate && !metadump.zero_stale_data)
		goto done;

	dirty = xlog_is_dirty(mp, &log);

	switch (dirty) {
	case 0:
		/* clear out a clean log */
		if (metadump.show_progress)
			print_progress("Zeroing clean log");

		logstart = XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart);
		logblocks = XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks);
		logversion = xfs_has_logv2(mp) ? 2 : 1;
		if (xfs_has_crc(mp))
			cycle = log.l_curr_cycle + 1;

		libxfs_log_clear(NULL, iocur_top->data, logstart, logblocks,
				 &mp->m_sb.sb_uuid, logversion,
				 mp->m_sb.sb_logsunit, XLOG_FMT, cycle, true);
		break;
	case 1:
		/* keep the dirty log */
		if (metadump.obfuscate)
			print_warning(
_("Warning: log recovery of an obfuscated metadata image can leak "
"unobfuscated metadata and/or cause image corruption.  If possible, "
"please mount the filesystem to clean the log, or disable obfuscation."));
		break;
	case -1:
		/* log detection error */
		if (metadump.obfuscate)
			print_warning(
_("Could not discern log; image will contain unobfuscated metadata in log."));
		break;
	}

done:
	return !write_buf(iocur_top);
}

static int
init_metadump_v1(void)
{
	metadump.metablock = (xfs_metablock_t *)calloc(BBSIZE + 1, BBSIZE);
	if (metadump.metablock == NULL) {
		print_warning("memory allocation failure");
		return -1;
	}
	metadump.metablock->mb_blocklog = BBSHIFT;
	metadump.metablock->mb_magic = cpu_to_be32(XFS_MD_MAGIC_V1);

	/* Set flags about state of metadump */
	metadump.metablock->mb_info = XFS_METADUMP_INFO_FLAGS;
	if (metadump.obfuscate)
		metadump.metablock->mb_info |= XFS_METADUMP_OBFUSCATED;
	if (!metadump.zero_stale_data)
		metadump.metablock->mb_info |= XFS_METADUMP_FULLBLOCKS;
	if (metadump.dirty_log)
		metadump.metablock->mb_info |= XFS_METADUMP_DIRTYLOG;

	metadump.block_index = (__be64 *)((char *)metadump.metablock +
				sizeof(xfs_metablock_t));
	metadump.block_buffer = (char *)(metadump.metablock) + BBSIZE;
	metadump.num_indices = (BBSIZE - sizeof(xfs_metablock_t)) / sizeof(__be64);

	/*
	 * A metadump block can hold at most num_indices of BBSIZE sectors;
	 * do not try to dump a filesystem with a sector size which does not
	 * fit within num_indices (i.e. within a single metablock).
	 */
	if (mp->m_sb.sb_sectsize > metadump.num_indices * BBSIZE) {
		print_warning("Cannot dump filesystem with sector size %u",
			      mp->m_sb.sb_sectsize);
		free(metadump.metablock);
		return -1;
	}

	metadump.cur_index = 0;

	return 0;
}

static int
finish_dump_metadump_v1(void)
{
	/*
	 * write index block and following data blocks (streaming)
	 */
	metadump.metablock->mb_count = cpu_to_be16(metadump.cur_index);
	if (fwrite(metadump.metablock, (metadump.cur_index + 1) << BBSHIFT, 1,
			metadump.outf) != 1) {
		print_warning("error writing to target file");
		return -1;
	}

	memset(metadump.block_index, 0, metadump.num_indices * sizeof(__be64));
	metadump.cur_index = 0;
	return 0;
}

static int
write_metadump_v1(
	enum typnm	type,
	const char	*data,
	xfs_daddr_t	off,
	int		len)
{
	int		i;
	int		ret;

	for (i = 0; i < len; i++, off++, data += BBSIZE) {
		metadump.block_index[metadump.cur_index] = cpu_to_be64(off);
		memcpy(&metadump.block_buffer[metadump.cur_index << BBSHIFT],
				data, BBSIZE);
		if (++metadump.cur_index == metadump.num_indices) {
			ret = finish_dump_metadump_v1();
			if (ret)
				return -EIO;
		}
	}

	return 0;
}

static void
release_metadump_v1(void)
{
	free(metadump.metablock);
}

static struct metadump_ops metadump1_ops = {
	.init		= init_metadump_v1,
	.write		= write_metadump_v1,
	.finish_dump	= finish_dump_metadump_v1,
	.release	= release_metadump_v1,
};

static int
init_metadump_v2(void)
{
	struct xfs_metadump_header	xmh = {0};
	uint32_t			compat_flags = 0;
	uint32_t			incompat_flags = 0;

	xmh.xmh_magic = cpu_to_be32(XFS_MD_MAGIC_V2);
	xmh.xmh_version = cpu_to_be32(2);

	if (metadump.obfuscate)
		compat_flags |= XFS_MD2_COMPAT_OBFUSCATED;
	if (!metadump.zero_stale_data)
		compat_flags |= XFS_MD2_COMPAT_FULLBLOCKS;
	if (metadump.dirty_log)
		compat_flags |= XFS_MD2_COMPAT_DIRTYLOG;
	if (metadump.external_log)
		compat_flags |= XFS_MD2_COMPAT_EXTERNALLOG;
	if (metadump.realtime_data)
		incompat_flags |= XFS_MD2_INCOMPAT_RTDEVICE;

	xmh.xmh_compat_flags = cpu_to_be32(compat_flags);
	xmh.xmh_incompat_flags = cpu_to_be32(incompat_flags);

	if (fwrite(&xmh, sizeof(xmh), 1, metadump.outf) != 1) {
		print_warning("error writing to target file");
		return -1;
	}

	return 0;
}

static int
copy_rtsupers(void)
{
	int		error;
	xfs_rtblock_t	rtbno;
	xfs_rgnumber_t	rgno = 0;

	if (metadump.show_progress)
		print_progress("Copying realtime superblocks");

	for (rgno = 0; rgno < mp->m_sb.sb_rgcount; rgno++) {
		rtbno = xfs_rgbno_to_rtb(mp, rgno, 0);

		push_cur();
		error = set_rt_cur(&typtab[TYP_RTSB],
				xfs_rtb_to_daddr(mp, rtbno),
				XFS_FSB_TO_BB(mp, 1), DB_RING_ADD, NULL);
		if (error)
			return 0;
		if (iocur_top->data == NULL) {
			pop_cur();
			print_warning("cannot read rt super %u", rgno);
			return !metadump.stop_on_read_error;
		}
		error = write_buf(iocur_top);
		pop_cur();
		if (error)
			return 0;
	}

	return 1;
}

static int
write_metadump_v2(
	enum typnm		type,
	const char		*data,
	xfs_daddr_t		off,
	int			len)
{
	struct xfs_meta_extent	xme;
	uint64_t		addr;

	addr = off;
	if (type == TYP_LOG &&
	    mp->m_logdev_targp->bt_bdev != mp->m_ddev_targp->bt_bdev)
		addr |= XME_ADDR_LOG_DEVICE;
	else if (type == TYP_RTSB)
		addr |= XME_ADDR_RT_DEVICE;
	else
		addr |= XME_ADDR_DATA_DEVICE;

	xme.xme_addr = cpu_to_be64(addr);
	xme.xme_len = cpu_to_be32(len);

	if (fwrite(&xme, sizeof(xme), 1, metadump.outf) != 1) {
		print_warning("error writing to target file");
		return -EIO;
	}

	if (fwrite(data, len << BBSHIFT, 1, metadump.outf) != 1) {
		print_warning("error writing to target file");
		return -EIO;
	}

	return 0;
}

static struct metadump_ops metadump2_ops = {
	.init	= init_metadump_v2,
	.write	= write_metadump_v2,
};

static int
metadump_f(
	int 		argc,
	char 		**argv)
{
	xfs_agnumber_t	agno;
	int		c;
	int		start_iocur_sp;
	int		outfd = -1;
	int		ret;
	char		*p;
	bool		version_opt_set = false;

	exitcode = 1;

	metadump.version = 1;
	metadump.show_progress = false;
	metadump.stop_on_read_error = false;
	metadump.max_extent_size = DEFAULT_MAX_EXT_SIZE;
	metadump.show_warnings = false;
	metadump.obfuscate = true;
	metadump.zero_stale_data = true;
	metadump.dirty_log = false;
	metadump.external_log = false;
	metadump.realtime_data = false;

	if (mp->m_sb.sb_magicnum != XFS_SB_MAGIC) {
		print_warning("bad superblock magic number %x, giving up",
				mp->m_sb.sb_magicnum);
		return 0;
	}

	/*
	 * on load, we sanity-checked agcount and possibly set to 1
	 * if it was corrupted and large.
	 */
	if (mp->m_sb.sb_agcount == 1 &&
	    XFS_MAX_DBLOCKS(&mp->m_sb) < mp->m_sb.sb_dblocks) {
		print_warning("truncated agcount, giving up");
		return 0;
	}

	while ((c = getopt(argc, argv, "aegm:ov:w")) != EOF) {
		switch (c) {
			case 'a':
				metadump.zero_stale_data = false;
				break;
			case 'e':
				metadump.stop_on_read_error = true;
				break;
			case 'g':
				metadump.show_progress = true;
				break;
			case 'm':
				metadump.max_extent_size =
					(int)strtol(optarg, &p, 0);
				if (*p != '\0' ||
				    metadump.max_extent_size <= 0) {
					print_warning("bad max extent size %s",
							optarg);
					return 0;
				}
				break;
			case 'o':
				metadump.obfuscate = false;
				break;
			case 'v':
				metadump.version = (int)strtol(optarg, &p, 0);
				if (*p != '\0' ||
				    (metadump.version != 1 &&
						metadump.version != 2)) {
					print_warning("bad metadump version: %s",
						optarg);
					return 0;
				}
				version_opt_set = true;
				break;
			case 'w':
				metadump.show_warnings = true;
				break;
			default:
				print_warning("bad option for metadump command");
				return 0;
		}
	}

	if (optind != argc - 1) {
		print_warning("too few options for metadump (no filename given)");
		return 0;
	}

	if (mp->m_logdev_targp->bt_bdev != mp->m_ddev_targp->bt_bdev)
		metadump.external_log = true;

	if (metadump.external_log && !version_opt_set)
		metadump.version = 2;

	if (metadump.version == 2 && mp->m_sb.sb_logstart == 0 &&
	    !metadump.external_log) {
		print_warning("external log device not loaded, use -l");
		return 1;
	}

	/* The realtime device only contains metadata if rtgroups is enabled. */
	if (mp->m_rtdev_targp->bt_bdev && xfs_has_rtgroups(mp))
		metadump.realtime_data = true;

	if (metadump.realtime_data && !version_opt_set)
		metadump.version = 2;

	if (metadump.version == 2 && xfs_has_realtime(mp) &&
	    xfs_has_rtgroups(mp) &&
	    !metadump.realtime_data) {
		print_warning("realtime device not loaded, use -R");
		return 1;
	}

	/*
	 * If we'll copy the log, see if the log is dirty.
	 *
	 * Metadump v1 does not support dumping the contents of an external
	 * log. Hence we skip the dirty log check.
	 */
	if (!(metadump.version == 1 && metadump.external_log)) {
		push_cur();
		if (metadump.external_log) {
			ASSERT(mp->m_sb.sb_logstart == 0);
			set_log_cur(&typtab[TYP_LOG],
					XFS_FSB_TO_DADDR(mp,
							mp->m_sb.sb_logstart),
					mp->m_sb.sb_logblocks * blkbb,
					DB_RING_IGN, NULL);
		} else {
			ASSERT(mp->m_sb.sb_logstart != 0);
			set_cur(&typtab[TYP_LOG],
					XFS_FSB_TO_DADDR(mp,
							mp->m_sb.sb_logstart),
					mp->m_sb.sb_logblocks * blkbb,
					DB_RING_IGN, NULL);
		}

		if (iocur_top->data) {	/* best effort */
			struct xlog	log;

			if (xlog_is_dirty(mp, &log))
				metadump.dirty_log = true;
		}
		pop_cur();
	}

	start_iocur_sp = iocur_sp;

	if (strcmp(argv[optind], "-") == 0) {
		if (isatty(fileno(stdout))) {
			print_warning("cannot write to a terminal");
			goto out;
		}
		/*
		 * Redirect stdout to stderr for the duration of the
		 * metadump operation so that dbprintf and other messages
		 * are sent to the console instead of polluting the
		 * metadump stream.
		 *
		 * We get to do this the hard way because musl doesn't
		 * allow reassignment of stdout.
		 */
		fflush(stdout);
		outfd = dup(STDOUT_FILENO);
		if (outfd < 0) {
			perror("opening dump stream");
			goto out;
		}
		ret = dup2(STDERR_FILENO, STDOUT_FILENO);
		if (ret < 0) {
			perror("redirecting stdout");
			close(outfd);
			goto out;
		}
		metadump.outf = fdopen(outfd, "a");
		if (metadump.outf == NULL) {
			fprintf(stderr, "cannot create dump stream\n");
			dup2(outfd, STDOUT_FILENO);
			close(outfd);
			goto out;
		}
		metadump.stdout_metadump = true;
	} else {
		metadump.outf = fopen(argv[optind], "wb");
		if (metadump.outf == NULL) {
			print_warning("cannot create dump file");
			goto out;
		}
	}

	if (metadump.version == 1)
		metadump.mdops = &metadump1_ops;
	else
		metadump.mdops = &metadump2_ops;

	ret = metadump.mdops->init();
	if (ret)
		goto out;

	exitcode = 0;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		if (!scan_ag(agno)) {
			exitcode = 1;
			break;
		}
	}

	/* copy realtime and quota inode contents */
	if (!exitcode)
		exitcode = !copy_sb_inodes();

	/* copy log */
	if (!exitcode && !(metadump.version == 1 && metadump.external_log))
		exitcode = !copy_log();

	/* copy rt sueprblocks */
	if (!exitcode && metadump.realtime_data) {
		if (!copy_rtsupers())
			exitcode = 1;
	}

	/* write the remaining index */
	if (!exitcode && metadump.mdops->finish_dump)
		exitcode = metadump.mdops->finish_dump() < 0;

	if (metadump.progress_since_warning)
		fputc('\n', metadump.stdout_metadump ? stderr : stdout);

	if (metadump.stdout_metadump) {
		fflush(metadump.outf);
		fflush(stdout);
		ret = dup2(outfd, STDOUT_FILENO);
		if (ret < 0)
			perror("un-redirecting stdout");
		metadump.stdout_metadump = false;
	}
	fclose(metadump.outf);

	/* cleanup iocur stack */
	while (iocur_sp > start_iocur_sp)
		pop_cur();

	if (metadump.mdops->release)
		metadump.mdops->release();

out:
	remaptable_clear();
	return 0;
}
