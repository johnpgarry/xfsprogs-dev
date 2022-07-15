// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <linux/fsmap.h>
#include "paths.h"
#include "fsgeom.h"
#include "logging.h"
#include "bulkstat.h"
#include "bitmap.h"
#include "file_exchange.h"
#include "clearspace.h"
#include "handle.h"

/*
 * Filesystem Space Balloons
 * =========================
 *
 * NOTE: Due to the evolving identity of this code, the "space_fd" or "space
 * file" in the codebase are the same as the balloon file in this introduction.
 * The introduction was written much later than the code.
 *
 * The goal of this code is to create a balloon file that is mapped to a range
 * of the physical space that is managed by a filesystem.  There are several
 * uses envisioned for balloon files:
 *
 * 1. Defragmenting free space.  Once the balloon is created, freeing it leaves
 *    a large chunk of contiguous free space ready for reallocation.
 *
 * 2. Shrinking the filesystem.  If the balloon is inflated at the end of the
 *    filesystem, the file can be handed to the shrink code.  The shrink code
 *    can then reduce the filesystem size by the size of the balloon.
 *
 * 3. Constraining usage of underlying thin provisioning pools.  The space
 *    assigned to a balloon can be DISCARDed, which prevents the filesystem
 *    from using that space until the balloon is freed.  This can be done more
 *    efficiently with the standard fallocate call, unless the balloon must
 *    target specific LBA ranges.
 *
 * Inflating a balloon is performed in five phases: claiming unused space;
 * freezing used space; migrating file mappings away from frozen space; moving
 * inodes; and rebuilding metadata elsewhere.
 *
 * Claiming Unused Space
 * ---------------------
 *
 * The first step of inflating a file balloon is to define the range of
 * physical space to be added to the balloon and claim as much of the free
 * space inside that range as possible.  Dirty data are flushed to disk and
 * the block and inode garbage collectors are run to remove any speculative
 * preallocations that might be occupying space in the target range.
 *
 * Second, the new XFS_IOC_MAP_FREESP ioctl is used to map free space in the
 * target range to the balloon file.  This step will be repeated after every
 * space-clearing step below to capture that cleared space.  Concurrent writer
 * threads will (hopefully) be allocated space outside the target range.
 *
 * Freezing Used Space
 * -------------------
 *
 * The second phase of inflating the balloon is to freeze as much of the
 * allocated space within the target range as possible.  The purpose of this
 * step is to grab a second reference to the used space, thereby preventing it
 * from being reused elsewhere.
 *
 * Freezing of a physical space extent starts by using GETFSMAP to find the
 * file owner of the space, and opening the file by handle.  The fsmap record
 * is used to create a FICLONERANGE request to link the file range into a work
 * file.  Once the reflink is made, any subsequent writes to any of the owners
 * of that space are staged via copy on write.  The balloon file prevents the
 * copy on write from being staged within the target range.  The frozen space
 * mapping is moved from the work file to the balloon file, where it remains
 * until the balloon file is freed.
 *
 * If reflink is not supported on the filesystem, used space cannot be frozen.
 * This phase is skipped.
 *
 * Migrating File Mappings
 * -----------------------
 *
 * Once the balloon file has been populated with as much of the target range as
 * possible, it is time to remap file ranges that point to the frozen space.
 *
 * It is advantageous to remap as many blocks as can be done with as few system
 * calls as possible to avoid fragmenting files.  Furthermore, it is preferable
 * to remap heavily shared extents before lightly shared extents to preserve
 * reflinks when possible.  The new GETFSREFCOUNTS call is used to rank
 * physical space extents by size and sharing factor so that the library always
 * tries to relocate the highest ranking space extent.
 *
 * Once a space extent has been selected for relocation, it is reflinked from
 * the balloon file into the work file.  Next, fallocate is called with the
 * FALLOC_FL_UNSHARE_RANGE mode to persist a new copy of the file data and
 * update the mapping in the work file.  The GETFSMAP call is used to find the
 * remaining owners of the target space.  For each owner, FIEDEDUPERANGE is
 * used to change the owner file's mapping to the space in the work file if the
 * owner has not been changed.
 *
 * If the filesystem does not support reflink, FIDEDUPERANGE will not be
 * available.  Fortunately, there will only be one owner of the frozen space.
 * The file range contents are instead copied through the page cache to the
 * work file, and EXCHANGE_RANGE is used to swap the mappings if the owner
 * file has not been modified.
 *
 * When the only remaining owner of the space is the balloon file, return to
 * the GETFSREFCOUNTS step to find a new target.  This phase is complete when
 * there are no more targets.
 *
 * Moving Inodes
 * -------------
 *
 * NOTE: This part is not written.
 *
 * When GETFSMAP tells us about an inode chunk, it is necessary to move the
 * inodes allocated in that inode chunk to a new chunk.  The first step is to
 * create a new donor file whose inode record is not in the target range.  This
 * file must be created in a donor directory.  Next, the file contents should
 * be cloned, either via FICLONE for regular files or by copying the directory
 * entries for directories.  The caller must ensure that no programs write to
 * the victim inode while this process is ongoing.
 *
 * Finally, the new inode must be mapped into the same points in the directory
 * tree as the old inode.  For each parent pointer accessible by the file,
 * perform a RENAME_EXCHANGE operation to update the directory entry.  One
 * obvious flaw of this method is that we cannot specify (parent, name, child)
 * pairs to renameat, which means that the rename does the wrong thing if
 * either directory is updated concurrently.
 *
 * If parent pointers are not available, this phase could be performed slowly
 * by iterating all directories looking for entries of interest and swapping
 * them.
 *
 * It is required that the caller guarantee that other applications cannot
 * update the filesystem concurrently.
 *
 * Rebuilding Metadata
 * -------------------
 *
 * The final phase identifies filesystem metadata occupying the target range
 * and uses the online filesystem repair facility to rebuild the metadata
 * structures.  Assuming that the balloon file now maps most of the space in
 * the target range, the new structures should be located outside of the target
 * range.  This phase runs in a loop until there is no more metadata to
 * relocate or no progress can be made on relocating metadata.
 *
 * Limitations and Bugs
 * --------------------
 *
 * - This code must be able to find the owners of a range of physical space.
 *   If GETFSMAP does not return owner information, this code cannot succeed.
 *   In other words, reverse mapping must be enabled.
 *
 * - We cannot freeze EOF blocks because the FICLONERANGE code does not allow
 *   us to remap an EOF block into the middle of the balloon file.  I think we
 *   actually succeed at reflinking the EOF block into the work file during the
 *   freeze step, but we need to dedupe/exchange the real owners' mappings
 *   without waiting for the freeze step.  OTOH, we /also/ want to freeze as
 *   much space as quickly as we can.
 *
 * - Freeze cannot use FIECLONERANGE to reflink unwritten extents into the work
 *   file because FICLONERANGE ignores unwritten extents.  We could create the
 *   work file as a sparse file and use EXCHANGE_RANGE to swap the unwritten
 *   extent with the hole, extend EOF to be allocunit aligned, and use
 *   EXCHANGE_RANGE to move it to the balloon file.  That first exchange must
 *   be careful to sample the owner file's bulkstat data, re-measure the file
 *   range to confirm that the unwritten extent is still the one we want, and
 *   only exchange if the owner file has not changed.
 *
 * - csp_buffercopy seems to hang if pread returns zero bytes read.  Do we dare
 *   use copy_file_range for this instead?
 *
 * - None of this code knows how to move inodes.  Phase 4 is entirely
 *   speculative fiction rooted in Dave Chinner's earlier implementation.
 *
 * - Does this work for realtime files?  Even for large rt extent sizes?
 */

/* VFS helpers */

/* Remap the file range described by @fcr into fd, or return an errno. */
static inline int
clonerange(int fd, struct file_clone_range *fcr)
{
	int	ret;

	ret = ioctl(fd, FICLONERANGE, fcr);
	if (ret)
		return errno;

	return 0;
}

/* Exchange the file ranges described by @xchg into fd, or return an errno. */
static inline int
exchangerange(int fd, struct xfs_exch_range *xchg)
{
	int	ret;

	ret = ioctl(fd, XFS_IOC_EXCHANGE_RANGE, xchg);
	if (ret)
		return errno;

	return 0;
}

/*
 * Deduplicate part of fd into the file range described by fdr.  If the
 * operation succeeded, we set @same to whether or not we deduped the data and
 * return zero.  If not, return an errno.
 */
static inline int
deduperange(int fd, struct file_dedupe_range *fdr, bool *same)
{
	struct file_dedupe_range_info *info = &fdr->info[0];
	int	ret;

	assert(fdr->dest_count == 1);
	*same = false;

	ret = ioctl(fd, FIDEDUPERANGE, fdr);
	if (ret)
		return errno;

	if (info->status < 0)
		return -info->status;

	if (info->status == FILE_DEDUPE_RANGE_DIFFERS)
		return 0;

	/* The kernel should never dedupe more than it was asked. */
	assert(fdr->src_length >= info->bytes_deduped);

	*same = true;
	return 0;
}

/* Space clearing operation control */

#define QUERY_BATCH_SIZE		1024

struct clearspace_tgt {
	unsigned long long	start;
	unsigned long long	length;
	unsigned long long	owners;
	unsigned long long	prio;
	unsigned long long	evacuated;
	bool			try_again;
};

struct clearspace_req {
	struct xfs_fd		*xfd;

	/* all the blocks that we've tried to clear */
	struct bitmap		*visited;

	/* stat buffer of the open file */
	struct stat		statbuf;
	struct stat		temp_statbuf;
	struct stat		space_statbuf;

	/* handle to this filesystem */
	void			*fshandle;
	size_t			fshandle_sz;

	/* physical storage that we want to clear */
	unsigned long long	start;
	unsigned long long	length;
	dev_t			dev;

	/* convenience variable */
	bool			realtime:1;
	bool			use_reflink:1;
	bool			can_evac_metadata:1;

	/*
	 * The "space capture" file.  Each extent in this file must be mapped
	 * to the same byte offset as the byte address of the physical space.
	 */
	int			space_fd;

	/* work file for migrating file data */
	int			work_fd;

	/* preallocated buffers for queries */
	struct getbmapx		*bhead;
	struct fsmap_head	*mhead;
	struct xfs_getfsrefs_head	*rhead;

	/* buffer for copying data */
	char			*buf;

	/* buffer for deduping data */
	struct file_dedupe_range *fdr;

	/* tracing mask and indent level */
	unsigned int		trace_mask;
	unsigned int		trace_indent;
};

static inline bool
csp_is_internal_owner(
	const struct clearspace_req	*req,
	unsigned long long		owner)
{
	return owner == req->temp_statbuf.st_ino ||
	       owner == req->space_statbuf.st_ino;
}

/* Debugging stuff */

static const struct csp_errstr {
	unsigned int		mask;
	const char		*tag;
} errtags[] = {
	{ CSP_TRACE_FREEZE,	"freeze" },
	{ CSP_TRACE_GRAB,	"grab" },
	{ CSP_TRACE_PREP,	"prep" },
	{ CSP_TRACE_TARGET,	"target" },
	{ CSP_TRACE_DEDUPE,	"dedupe" },
	{ CSP_TRACE_EXCHANGE,	"exchange_range" },
	{ CSP_TRACE_XREBUILD,	"rebuild" },
	{ CSP_TRACE_EFFICACY,	"efficacy" },
	{ CSP_TRACE_SETUP,	"setup" },
	{ CSP_TRACE_DUMPFILE,	"dumpfile" },
	{ CSP_TRACE_BITMAP,	"bitmap" },

	/* prioritize high level functions over low level queries for tagging */
	{ CSP_TRACE_FSMAP,	"fsmap" },
	{ CSP_TRACE_FSREFS,	"fsrefs" },
	{ CSP_TRACE_BMAPX,	"bmapx" },
	{ CSP_TRACE_FALLOC,	"falloc" },
	{ CSP_TRACE_STATUS,	"status" },
	{ 0, NULL },
};

static void
csp_debug(
	struct clearspace_req	*req,
	unsigned int		mask,
	const char		*func,
	int			line,
	const char		*format,
	...)
{
	const struct csp_errstr	*et = errtags;
	bool			debug = (req->trace_mask & ~CSP_TRACE_STATUS);
	int			indent = req->trace_indent;
	va_list			args;

	if ((req->trace_mask & mask) != mask)
		return;

	if (debug) {
		while (indent > 0) {
			fprintf(stderr, "  ");
			indent--;
		}

		for (; et->tag; et++) {
			if (et->mask & mask) {
				fprintf(stderr, "%s: ", et->tag);
				break;
			}
		}
	}

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);

	if (debug)
		fprintf(stderr, " (line %d)\n", line);
	else
		fprintf(stderr, "\n");
	fflush(stderr);
}

#define trace_freeze(req, format, ...)	\
	csp_debug((req), CSP_TRACE_FREEZE, __func__, __LINE__, format, __VA_ARGS__)

#define trace_grabfree(req, format, ...)	\
	csp_debug((req), CSP_TRACE_GRAB, __func__, __LINE__, format, __VA_ARGS__)

#define trace_fsmap(req, format, ...)	\
	csp_debug((req), CSP_TRACE_FSMAP, __func__, __LINE__, format, __VA_ARGS__)

#define trace_fsmap_rec(req, mask, mrec)	\
	while (!csp_is_internal_owner((req), (mrec)->fmr_owner)) { \
		csp_debug((req), (mask) | CSP_TRACE_FSMAP, __func__, __LINE__, \
"fsmap phys 0x%llx owner 0x%llx offset 0x%llx bytecount 0x%llx flags 0x%x", \
				(unsigned long long)(mrec)->fmr_physical, \
				(unsigned long long)(mrec)->fmr_owner, \
				(unsigned long long)(mrec)->fmr_offset, \
				(unsigned long long)(mrec)->fmr_length, \
				(mrec)->fmr_flags); \
		break; \
	}

#define trace_fsrefs(req, format, ...)	\
	csp_debug((req), CSP_TRACE_FSREFS, __func__, __LINE__, format, __VA_ARGS__)

#define trace_fsrefs_rec(req, mask, rrec)	\
	csp_debug((req), (mask) | CSP_TRACE_FSREFS, __func__, __LINE__, \
"fsref phys 0x%llx bytecount 0x%llx owners %llu flags 0x%x", \
			(unsigned long long)(rrec)->fcr_physical, \
			(unsigned long long)(rrec)->fcr_length, \
			(unsigned long long)(rrec)->fcr_owners, \
			(rrec)->fcr_flags)

#define trace_bmapx(req, format, ...)	\
	csp_debug((req), CSP_TRACE_BMAPX, __func__, __LINE__, format, __VA_ARGS__)

#define trace_bmapx_rec(req, mask, brec)	\
	csp_debug((req), (mask) | CSP_TRACE_BMAPX, __func__, __LINE__, \
"bmapx pos 0x%llx bytecount 0x%llx phys 0x%llx flags 0x%x", \
			(unsigned long long)BBTOB((brec)->bmv_offset), \
			(unsigned long long)BBTOB((brec)->bmv_length), \
			(unsigned long long)BBTOB((brec)->bmv_block), \
			(brec)->bmv_oflags)

#define trace_prep(req, format, ...)	\
	csp_debug((req), CSP_TRACE_PREP, __func__, __LINE__, format, __VA_ARGS__)

#define trace_target(req, format, ...)	\
	csp_debug((req), CSP_TRACE_TARGET, __func__, __LINE__, format, __VA_ARGS__)

#define trace_dedupe(req, format, ...)	\
	csp_debug((req), CSP_TRACE_DEDUPE, __func__, __LINE__, format, __VA_ARGS__)

#define trace_falloc(req, format, ...)	\
	csp_debug((req), CSP_TRACE_FALLOC, __func__, __LINE__, format, __VA_ARGS__)

#define trace_exchange(req, format, ...)	\
	csp_debug((req), CSP_TRACE_EXCHANGE, __func__, __LINE__, format, __VA_ARGS__)

#define trace_xrebuild(req, format, ...)	\
	csp_debug((req), CSP_TRACE_XREBUILD, __func__, __LINE__, format, __VA_ARGS__)

#define trace_setup(req, format, ...)	\
	csp_debug((req), CSP_TRACE_SETUP, __func__, __LINE__, format, __VA_ARGS__)

#define trace_status(req, format, ...)	\
	csp_debug((req), CSP_TRACE_STATUS, __func__, __LINE__, format, __VA_ARGS__)

#define trace_dumpfile(req, format, ...)	\
	csp_debug((req), CSP_TRACE_DUMPFILE, __func__, __LINE__, format, __VA_ARGS__)

#define trace_bitmap(req, format, ...)	\
	csp_debug((req), CSP_TRACE_BITMAP, __func__, __LINE__, format, __VA_ARGS__)

/* VFS Iteration helpers */

static inline void
start_spacefd_iter(struct clearspace_req *req)
{
	req->trace_indent++;
}

static inline void
end_spacefd_iter(struct clearspace_req *req)
{
	req->trace_indent--;
}

/*
 * Iterate each hole in the space-capture file.  Returns 1 if holepos/length
 * has been set to a hole; 0 if there aren't any holes left, or -1 for error.
 */
static inline int
spacefd_hole_iter(
	const struct clearspace_req	*req,
	loff_t			*holepos,
	loff_t			*length)
{
	loff_t			end = req->start + req->length;
	loff_t			h;
	loff_t			d;

	if (*length == 0)
		d = req->start;
	else
		d = *holepos + *length;
	if (d >= end)
		return 0;

	h = lseek(req->space_fd, d, SEEK_HOLE);
	if (h < 0) {
		perror(_("finding start of hole in space capture file"));
		return h;
	}
	if (h >= end)
		return 0;

	d = lseek(req->space_fd, h, SEEK_DATA);
	if (d < 0 && errno == ENXIO)
		d = end;
	if (d < 0) {
		perror(_("finding end of hole in space capture file"));
		return d;
	}
	if (d > end)
		d = end;

	*holepos = h;
	*length = d - h;
	return 1;
}

/*
 * Iterate each written region in the space-capture file.  Returns 1 if
 * datapos/length have been set to a data area; 0 if there isn't any data left,
 * or -1 for error.
 */
static int
spacefd_data_iter(
	const struct clearspace_req	*req,
	loff_t			*datapos,
	loff_t			*length)
{
	loff_t			end = req->start + req->length;
	loff_t			d;
	loff_t			h;

	if (*length == 0)
		h = req->start;
	else
		h = *datapos + *length;
	if (h >= end)
		return 0;

	d = lseek(req->space_fd, h, SEEK_DATA);
	if (d < 0 && errno == ENXIO)
		return 0;
	if (d < 0) {
		perror(_("finding start of data in space capture file"));
		return d;
	}
	if (d >= end)
		return 0;

	h = lseek(req->space_fd, d, SEEK_HOLE);
	if (h < 0) {
		perror(_("finding end of data in space capture file"));
		return h;
	}
	if (h > end)
		h = end;

	*datapos = d;
	*length = h - d;
	return 1;
}

/* Filesystem space usage queries */

/* Allocate the structures needed for a fsmap query. */
static void
start_fsmap_query(
	struct clearspace_req	*req,
	dev_t			dev,
	unsigned long long	physical,
	unsigned long long	length)
{
	struct fsmap_head	*mhead = req->mhead;

	assert(req->mhead->fmh_count == 0);
	memset(mhead, 0, sizeof(struct fsmap_head));
	mhead->fmh_count = QUERY_BATCH_SIZE;
	mhead->fmh_keys[0].fmr_device = dev;
	mhead->fmh_keys[0].fmr_physical = physical;
	mhead->fmh_keys[1].fmr_device = dev;
	mhead->fmh_keys[1].fmr_physical = physical + length;
	mhead->fmh_keys[1].fmr_owner = ULLONG_MAX;
	mhead->fmh_keys[1].fmr_flags = UINT_MAX;
	mhead->fmh_keys[1].fmr_offset = ULLONG_MAX;

	trace_fsmap(req, "dev %u:%u physical 0x%llx bytecount 0x%llx highkey 0x%llx",
			major(dev), minor(dev),
			(unsigned long long)physical,
			(unsigned long long)length,
			(unsigned long long)mhead->fmh_keys[1].fmr_physical);
	req->trace_indent++;
}

static inline void
end_fsmap_query(
	struct clearspace_req	*req)
{
	req->trace_indent--;
	req->mhead->fmh_count = 0;
}

/* Set us up for the next run_fsmap_query, or return false. */
static inline bool
advance_fsmap_cursor(struct fsmap_head *mhead)
{
	struct fsmap	*mrec;

	mrec = &mhead->fmh_recs[mhead->fmh_entries - 1];
	if (mrec->fmr_flags & FMR_OF_LAST)
		return false;

	fsmap_advance(mhead);
	return true;
}

/*
 * Run a GETFSMAP query.  Returns 1 if there are rows, 0 if there are no rows,
 * or -1 for error.
 */
static inline int
run_fsmap_query(
	struct clearspace_req	*req)
{
	struct fsmap_head	*mhead = req->mhead;
	int			ret;

	if (mhead->fmh_entries > 0 && !advance_fsmap_cursor(mhead))
		return 0;

	trace_fsmap(req,
 "ioctl dev %u:%u physical 0x%llx length 0x%llx highkey 0x%llx",
			major(mhead->fmh_keys[0].fmr_device),
			minor(mhead->fmh_keys[0].fmr_device),
			(unsigned long long)mhead->fmh_keys[0].fmr_physical,
			(unsigned long long)mhead->fmh_keys[0].fmr_length,
			(unsigned long long)mhead->fmh_keys[1].fmr_physical);

	ret = ioctl(req->xfd->fd, FS_IOC_GETFSMAP, mhead);
	if (ret) {
		perror(_("querying fsmap data"));
		return -1;
	}

	if (!(mhead->fmh_oflags & FMH_OF_DEV_T)) {
		fprintf(stderr, _("fsmap does not return dev_t.\n"));
		return -1;
	}

	if (mhead->fmh_entries == 0)
		return 0;

	return 1;
}

#define for_each_fsmap_row(req, rec) \
	for ((rec) = (req)->mhead->fmh_recs; \
	     (rec) < (req)->mhead->fmh_recs + (req)->mhead->fmh_entries; \
	     (rec)++)

/* Allocate the structures needed for a fsrefcounts query. */
static void
start_fsrefs_query(
	struct clearspace_req	*req,
	dev_t			dev,
	unsigned long long	physical,
	unsigned long long	length)
{
	struct xfs_getfsrefs_head	*rhead = req->rhead;

	assert(req->rhead->fch_count == 0);
	memset(rhead, 0, sizeof(struct xfs_getfsrefs_head));
	rhead->fch_count = QUERY_BATCH_SIZE;
	rhead->fch_keys[0].fcr_device = dev;
	rhead->fch_keys[0].fcr_physical = physical;
	rhead->fch_keys[1].fcr_device = dev;
	rhead->fch_keys[1].fcr_physical = physical + length;
	rhead->fch_keys[1].fcr_owners = ULLONG_MAX;
	rhead->fch_keys[1].fcr_flags = UINT_MAX;

	trace_fsrefs(req, "dev %u:%u physical 0x%llx bytecount 0x%llx highkey 0x%llx",
			major(dev), minor(dev),
			(unsigned long long)physical,
			(unsigned long long)length,
			(unsigned long long)rhead->fch_keys[1].fcr_physical);
	req->trace_indent++;
}

static inline void
end_fsrefs_query(
	struct clearspace_req	*req)
{
	req->trace_indent--;
	req->rhead->fch_count = 0;
}

/* Set us up for the next run_fsrefs_query, or return false. */
static inline bool
advance_fsrefs_query(struct xfs_getfsrefs_head *rhead)
{
	struct xfs_getfsrefs	*rrec;

	rrec = &rhead->fch_recs[rhead->fch_entries - 1];
	if (rrec->fcr_flags & FCR_OF_LAST)
		return false;

	xfs_getfsrefs_advance(rhead);
	return true;
}

/*
 * Run a GETFSREFCOUNTS query.  Returns 1 if there are rows, 0 if there are
 * no rows, or -1 for error.
 */
static inline int
run_fsrefs_query(
	struct clearspace_req	*req)
{
	struct xfs_getfsrefs_head	*rhead = req->rhead;
	int			ret;

	if (rhead->fch_entries > 0 && !advance_fsrefs_query(rhead))
		return 0;

	trace_fsrefs(req,
 "ioctl dev %u:%u physical 0x%llx length 0x%llx highkey 0x%llx",
			major(rhead->fch_keys[0].fcr_device),
			minor(rhead->fch_keys[0].fcr_device),
			(unsigned long long)rhead->fch_keys[0].fcr_physical,
			(unsigned long long)rhead->fch_keys[0].fcr_length,
			(unsigned long long)rhead->fch_keys[1].fcr_physical);

	ret = ioctl(req->xfd->fd, XFS_IOC_GETFSREFCOUNTS, rhead);
	if (ret) {
		perror(_("querying refcount data"));
		return -1;
	}

	if (!(rhead->fch_oflags & FCH_OF_DEV_T)) {
		fprintf(stderr, _("fsrefcounts does not return dev_t.\n"));
		return -1;
	}

	if (rhead->fch_entries == 0)
		return 0;

	return 1;
}

#define for_each_fsref_row(req, rec) \
	for ((rec) = (req)->rhead->fch_recs; \
	     (rec) < (req)->rhead->fch_recs + (req)->rhead->fch_entries; \
	     (rec)++)

/* Allocate the structures needed for a bmapx query. */
static void
start_bmapx_query(
	struct clearspace_req	*req,
	unsigned int		fork,
	unsigned long long	pos,
	unsigned long long	length)
{
	struct getbmapx		*bhead = req->bhead;

	assert(fork == BMV_IF_ATTRFORK || fork == BMV_IF_COWFORK || !fork);
	assert(req->bhead->bmv_count == 0);

	memset(bhead, 0, sizeof(struct getbmapx));
	bhead[0].bmv_offset = BTOBB(pos);
	bhead[0].bmv_length = BTOBB(length);
	bhead[0].bmv_count = QUERY_BATCH_SIZE + 1;
	bhead[0].bmv_iflags = fork | BMV_IF_PREALLOC | BMV_IF_DELALLOC;

	trace_bmapx(req, "%s pos 0x%llx bytecount 0x%llx",
			fork == BMV_IF_COWFORK ? "cow" : fork == BMV_IF_ATTRFORK ? "attr" : "data",
			(unsigned long long)BBTOB(bhead[0].bmv_offset),
			(unsigned long long)BBTOB(bhead[0].bmv_length));
	req->trace_indent++;
}

static inline void
end_bmapx_query(
	struct clearspace_req	*req)
{
	req->trace_indent--;
	req->bhead->bmv_count = 0;
}

/* Set us up for the next run_bmapx_query, or return false. */
static inline bool
advance_bmapx_query(struct getbmapx *bhead)
{
	struct getbmapx		*brec;
	unsigned long long	next_offset;
	unsigned long long	end = bhead->bmv_offset + bhead->bmv_length;

	brec = &bhead[bhead->bmv_entries];
	if (brec->bmv_oflags & BMV_OF_LAST)
		return false;

	next_offset = brec->bmv_offset + brec->bmv_length;
	if (next_offset > end)
		return false;

	bhead->bmv_offset = next_offset;
	bhead->bmv_length = end - next_offset;
	return true;
}

/*
 * Run a GETBMAPX query.  Returns 1 if there are rows, 0 if there are no rows,
 * or -1 for error.
 */
static inline int
run_bmapx_query(
	struct clearspace_req	*req,
	int			fd)
{
	struct getbmapx		*bhead = req->bhead;
	unsigned int		fork;
	int			ret;

	if (bhead->bmv_entries > 0 && !advance_bmapx_query(bhead))
		return 0;

	fork = bhead[0].bmv_iflags & (BMV_IF_COWFORK | BMV_IF_ATTRFORK);
	trace_bmapx(req, "ioctl %s pos 0x%llx bytecount 0x%llx",
			fork == BMV_IF_COWFORK ? "cow" : fork == BMV_IF_ATTRFORK ? "attr" : "data",
			(unsigned long long)BBTOB(bhead[0].bmv_offset),
			(unsigned long long)BBTOB(bhead[0].bmv_length));

	ret = ioctl(fd, XFS_IOC_GETBMAPX, bhead);
	if (ret) {
		perror(_("querying bmapx data"));
		return -1;
	}

	if (bhead->bmv_entries == 0)
		return 0;

	return 1;
}

#define for_each_bmapx_row(req, rec) \
	for ((rec) = (req)->bhead + 1; \
	     (rec) < (req)->bhead + 1 + (req)->bhead->bmv_entries; \
	     (rec)++)

static inline void
csp_dump_bmapx_row(
	struct clearspace_req	*req,
	unsigned int		nr,
	const struct getbmapx	*brec)
{
	if (brec->bmv_block == -1) {
		trace_dumpfile(req, "[%u]: pos 0x%llx len 0x%llx hole",
				nr,
				(unsigned long long)BBTOB(brec->bmv_offset),
				(unsigned long long)BBTOB(brec->bmv_length));
		return;
	}

	if (brec->bmv_block == -2) {
		trace_dumpfile(req, "[%u]: pos 0x%llx len 0x%llx delalloc",
				nr,
				(unsigned long long)BBTOB(brec->bmv_offset),
				(unsigned long long)BBTOB(brec->bmv_length));
		return;
	}

	trace_dumpfile(req, "[%u]: pos 0x%llx len 0x%llx phys 0x%llx flags 0x%x",
			nr,
			(unsigned long long)BBTOB(brec->bmv_offset),
			(unsigned long long)BBTOB(brec->bmv_length),
			(unsigned long long)BBTOB(brec->bmv_block),
			brec->bmv_oflags);
}

static inline void
csp_dump_bmapx(
	struct clearspace_req	*req,
	int			fd,
	unsigned int		indent,
	const char		*tag)
{
	unsigned int		nr;
	int			ret;

	trace_dumpfile(req, "DUMP BMAP OF DATA FORK %s", tag);
	start_bmapx_query(req, 0, req->start, req->length);
	nr = 0;
	while ((ret = run_bmapx_query(req, fd)) > 0) {
		struct getbmapx	*brec;

		for_each_bmapx_row(req, brec) {
			csp_dump_bmapx_row(req, nr++, brec);
			if (nr > 10)
				goto dump_cow;
		}
	}

dump_cow:
	end_bmapx_query(req);
	trace_dumpfile(req, "DUMP BMAP OF COW FORK %s", tag);
	start_bmapx_query(req, BMV_IF_COWFORK, req->start, req->length);
	nr = 0;
	while ((ret = run_bmapx_query(req, fd)) > 0) {
		struct getbmapx	*brec;

		for_each_bmapx_row(req, brec) {
			csp_dump_bmapx_row(req, nr++, brec);
			if (nr > 10)
				goto dump_attr;
		}
	}

dump_attr:
	end_bmapx_query(req);
	trace_dumpfile(req, "DUMP BMAP OF ATTR FORK %s", tag);
	start_bmapx_query(req, BMV_IF_ATTRFORK, req->start, req->length);
	nr = 0;
	while ((ret = run_bmapx_query(req, fd)) > 0) {
		struct getbmapx	*brec;

		for_each_bmapx_row(req, brec) {
			csp_dump_bmapx_row(req, nr++, brec);
			if (nr > 10)
				goto stop;
		}
	}

stop:
	end_bmapx_query(req);
	trace_dumpfile(req, "DONE DUMPING %s", tag);
}

/* Return the first bmapx for the given file range. */
static int
bmapx_one(
	struct clearspace_req	*req,
	int			fd,
	unsigned long long	pos,
	unsigned long long	length,
	struct getbmapx		*brec)
{
	struct getbmapx		bhead[2];
	int			ret;

	memset(bhead, 0, sizeof(struct getbmapx) * 2);
	bhead[0].bmv_offset = BTOBB(pos);
	bhead[0].bmv_length = BTOBB(length);
	bhead[0].bmv_count = 2;
	bhead[0].bmv_iflags = BMV_IF_PREALLOC | BMV_IF_DELALLOC;

	ret = ioctl(fd, XFS_IOC_GETBMAPX, bhead);
	if (ret) {
		perror(_("simple bmapx query"));
		return -1;
	}

	if (bhead->bmv_entries > 0) {
		memcpy(brec, &bhead[1], sizeof(struct getbmapx));
		return 0;
	}

	memset(brec, 0, sizeof(struct getbmapx));
	brec->bmv_offset = pos;
	brec->bmv_block = -1;	/* hole */
	brec->bmv_length = length;
	return 0;
}

/* Constrain space map records. */
static void
__trim_fsmap(
	uint64_t		start,
	uint64_t		length,
	struct fsmap		*fsmap)
{
	unsigned long long	delta, end;
	bool			need_off;

	need_off = (fsmap->fmr_flags & (FMR_OF_EXTENT_MAP |
					FMR_OF_SPECIAL_OWNER));

	if (fsmap->fmr_physical < start) {
		delta = start - fsmap->fmr_physical;
		fsmap->fmr_physical = start;
		fsmap->fmr_length -= delta;
		if (need_off)
			fsmap->fmr_offset += delta;
	}

	end = fsmap->fmr_physical + fsmap->fmr_length;
	if (end > start + length) {
		delta = end - (start + length);
		fsmap->fmr_length -= delta;
	}
}

static inline void
trim_target_fsmap(const struct clearspace_tgt *tgt, struct fsmap *fsmap)
{
	return __trim_fsmap(tgt->start, tgt->length, fsmap);
}

static inline void
trim_request_fsmap(const struct clearspace_req *req, struct fsmap *fsmap)
{
	return __trim_fsmap(req->start, req->length, fsmap);
}

/* Actual space clearing code */

/*
 * Map all the free space in the region that we're clearing to the space
 * catcher file.
 */
static int
csp_grab_free_space(
	struct clearspace_req	*req)
{
	struct xfs_map_freesp	args = {
		.offset		= req->start,
		.len		= req->length,
	};
	int			ret;

	trace_grabfree(req, "start 0x%llx length 0x%llx",
			(unsigned long long)req->start,
			(unsigned long long)req->length);

	ret = ioctl(req->space_fd, XFS_IOC_MAP_FREESP, &args);
	if (ret) {
		perror(_("map free space to space capture file"));
		return -1;
	}

	return 0;
}

/*
 * Rank a refcount record.  We prefer to tackle highly shared and longer
 * extents first.
 */
static inline unsigned long long
csp_space_prio(
	const struct xfs_fsop_geom	*g,
	const struct xfs_getfsrefs	*p)
{
	unsigned long long		blocks = p->fcr_length / g->blocksize;
	unsigned long long		ret = blocks * p->fcr_owners;

	if (ret < blocks || ret < p->fcr_owners)
		return UINT64_MAX;
	return ret;
}

/* Make the current refcount record the clearing target if desirable. */
static void
csp_adjust_target(
	struct clearspace_req		*req,
	struct clearspace_tgt		*target,
	const struct xfs_getfsrefs	*rec,
	unsigned long long		prio)
{
	if (prio < target->prio)
		return;
	if (prio == target->prio &&
	    rec->fcr_length <= target->length)
		return;

	/* Ignore results that go beyond the end of what we wanted. */
	if (rec->fcr_physical >= req->start + req->length)
		return;

	/* Ignore regions that we already tried to clear. */
	if (bitmap_test(req->visited, rec->fcr_physical, rec->fcr_length))
		return;

	trace_target(req,
 "set target, prio 0x%llx -> 0x%llx phys 0x%llx bytecount 0x%llx",
			target->prio, prio,
			(unsigned long long)rec->fcr_physical,
			(unsigned long long)rec->fcr_length);

	target->start = rec->fcr_physical;
	target->length = rec->fcr_length;
	target->owners = rec->fcr_owners;
	target->prio = prio;
}

/*
 * Decide if this refcount record maps to extents that are sufficiently
 * interesting to target.
 */
static int
csp_evaluate_refcount(
	struct clearspace_req		*req,
	const struct xfs_getfsrefs	*rrec,
	struct clearspace_tgt		*target)
{
	const struct xfs_fsop_geom	*fsgeom = &req->xfd->fsgeom;
	unsigned long long		prio = csp_space_prio(fsgeom, rrec);
	int				ret;

	if (rrec->fcr_device != req->dev)
		return 0;

	if (prio < target->prio)
		return 0;

	/*
	 * XFS only supports sharing data blocks.  If there's more than one
	 * owner, we know that we can easily move the blocks.
	 */
	if (rrec->fcr_owners > 1) {
		csp_adjust_target(req, target, rrec, prio);
		return 0;
	}

	/*
	 * Otherwise, this extent has single owners.  Walk the fsmap records to
	 * figure out if they're movable or not.
	 */
	start_fsmap_query(req, rrec->fcr_device, rrec->fcr_physical,
			rrec->fcr_length);
	while ((ret = run_fsmap_query(req)) > 0) {
		struct fsmap	*mrec;
		uint64_t	next_phys = 0;

		for_each_fsmap_row(req, mrec) {
			struct xfs_getfsrefs	fake_rec = { };

			trace_fsmap_rec(req, CSP_TRACE_TARGET, mrec);

			if (mrec->fmr_device != rrec->fcr_device)
				continue;
			if (mrec->fmr_flags & FMR_OF_SPECIAL_OWNER)
				continue;
			if (csp_is_internal_owner(req, mrec->fmr_owner))
				continue;

			/*
			 * If the space has become shared since the fsrefs
			 * query, just skip this record.  We might come back to
			 * it in a later iteration.
			 */
			if (mrec->fmr_physical < next_phys)
				continue;

			/* Fake enough of a fsrefs to calculate the priority. */
			fake_rec.fcr_physical = mrec->fmr_physical;
			fake_rec.fcr_length = mrec->fmr_length;
			fake_rec.fcr_owners = 1;
			prio = csp_space_prio(fsgeom, &fake_rec);

			/* Target unwritten extents first; they're cheap. */
			if (mrec->fmr_flags & FMR_OF_PREALLOC)
				prio |= (1ULL << 63);

			csp_adjust_target(req, target, &fake_rec, prio);

			next_phys = mrec->fmr_physical + mrec->fmr_length;
		}
	}
	end_fsmap_query(req);

	return ret;
}

/*
 * Given a range of storage to search, find the most appealing target for space
 * clearing.  If nothing suitable is found, the target will be zeroed.
 */
static int
csp_find_target(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target)
{
	int			ret;

	memset(target, 0, sizeof(struct clearspace_tgt));

	start_fsrefs_query(req, req->dev, req->start, req->length);
	while ((ret = run_fsrefs_query(req)) > 0) {
		struct xfs_getfsrefs	*rrec;

		for_each_fsref_row(req, rrec) {
			trace_fsrefs_rec(req, CSP_TRACE_TARGET, rrec);
			ret = csp_evaluate_refcount(req, rrec, target);
			if (ret) {
				end_fsrefs_query(req);
				return ret;
			}
		}
	}
	end_fsrefs_query(req);

	if (target->length != 0) {
		/*
		 * Mark this extent visited so that we won't try again this
		 * round.
		 */
		trace_bitmap(req, "set filedata start 0x%llx length 0x%llx",
				target->start, target->length);
		ret = bitmap_set(req->visited, target->start, target->length);
		if (ret) {
			perror(_("marking file extent visited"));
			return ret;
		}
	}

	return 0;
}

/* Try to evacuate blocks by using online repair. */
static int
csp_evac_file_metadata(
	struct clearspace_req		*req,
	struct clearspace_tgt		*target,
	const struct fsmap		*mrec,
	int				fd,
	const struct xfs_bulkstat	*bulkstat)
{
	struct xfs_scrub_metadata	scrub = {
		.sm_type		= XFS_SCRUB_TYPE_PROBE,
		.sm_flags		= XFS_SCRUB_IFLAG_REPAIR |
					  XFS_SCRUB_IFLAG_FORCE_REBUILD,
	};
	struct xfs_fd			*xfd = req->xfd;
	int				ret;

	trace_xrebuild(req,
 "ino 0x%llx pos 0x%llx bytecount 0x%llx phys 0x%llx flags 0x%llx",
				(unsigned long long)mrec->fmr_owner,
				(unsigned long long)mrec->fmr_offset,
				(unsigned long long)mrec->fmr_physical,
				(unsigned long long)mrec->fmr_length,
				(unsigned long long)mrec->fmr_flags);

	if (fd == -1) {
		scrub.sm_ino = mrec->fmr_owner;
		scrub.sm_gen = bulkstat->bs_gen;
		fd = xfd->fd;
	}

	if (mrec->fmr_flags & FMR_OF_ATTR_FORK) {
		if (mrec->fmr_flags & FMR_OF_EXTENT_MAP)
			scrub.sm_type = XFS_SCRUB_TYPE_BMBTA;
		else
			scrub.sm_type = XFS_SCRUB_TYPE_XATTR;
	} else if (mrec->fmr_flags & FMR_OF_EXTENT_MAP) {
		scrub.sm_type = XFS_SCRUB_TYPE_BMBTD;
	} else if (S_ISLNK(bulkstat->bs_mode)) {
		scrub.sm_type = XFS_SCRUB_TYPE_SYMLINK;
	} else if (S_ISDIR(bulkstat->bs_mode)) {
		scrub.sm_type = XFS_SCRUB_TYPE_DIR;
	}

	if (scrub.sm_type == XFS_SCRUB_TYPE_PROBE)
		return 0;

	trace_xrebuild(req, "ino 0x%llx gen 0x%x type %u",
			(unsigned long long)mrec->fmr_owner,
			(unsigned int)bulkstat->bs_gen,
			(unsigned int)scrub.sm_type);

	ret = ioctl(fd, XFS_IOC_SCRUB_METADATA, &scrub);
	if (ret) {
		fprintf(stderr,
	_("evacuating inode 0x%llx metadata type %u: %s\n"),
				(unsigned long long)mrec->fmr_owner,
				scrub.sm_type, strerror(errno));
		return -1;
	}

	target->evacuated++;
	return 0;
}

/*
 * Open an inode via handle.  Returns a file descriptor, -2 if the file is
 * gone, or -1 on error.
 */
static int
csp_open_by_handle(
	struct clearspace_req	*req,
	int			oflags,
	uint64_t		ino,
	uint32_t		gen)
{
	struct xfs_handle	handle = { };
	struct xfs_fsop_handlereq hreq = {
		.oflags		= oflags | O_NOATIME | O_NOFOLLOW |
				  O_NOCTTY | O_LARGEFILE,
		.ihandle	= &handle,
		.ihandlen	= sizeof(handle),
	};
	int			ret;

	memcpy(&handle.ha_fsid, req->fshandle, sizeof(handle.ha_fsid));
	handle.ha_fid.fid_len = sizeof(xfs_fid_t) -
			sizeof(handle.ha_fid.fid_len);
	handle.ha_fid.fid_pad = 0;
	handle.ha_fid.fid_ino = ino;
	handle.ha_fid.fid_gen = gen;

	/*
	 * Since we extracted the fshandle from the open file instead of using
	 * path_to_fshandle, the fsid cache doesn't know about the fshandle.
	 * Construct the open by handle request manually.
	 */
	ret = ioctl(req->xfd->fd, XFS_IOC_OPEN_BY_HANDLE, &hreq);
	if (ret < 0) {
		if (errno == ENOENT || errno == EINVAL)
			return -2;

		fprintf(stderr, _("open inode 0x%llx: %s\n"),
				(unsigned long long)ino,
				strerror(errno));
		return -1;
	}

	return ret;
}

/*
 * Open a file for evacuation.  Returns a positive errno on error; a fd in @fd
 * if the caller is supposed to do something; or @fd == -1 if there's nothing
 * further to do.
 */
static int
csp_evac_open(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target,
	const struct fsmap	*mrec,
	struct xfs_bulkstat	*bulkstat,
	int			oflags,
	int			*fd)
{
	struct xfs_bulkstat	__bs;
	int			target_fd;
	int			ret;

	*fd = -1;

	if (csp_is_internal_owner(req, mrec->fmr_owner) ||
	    (mrec->fmr_flags & FMR_OF_SPECIAL_OWNER))
		goto nothing_to_do;

	if (bulkstat == NULL)
		bulkstat = &__bs;

	/*
	 * Snapshot this file so that we can perform a fresh-only exchange.
	 * For other types of files we just skip to the evacuation step.
	 */
	ret = -xfrog_bulkstat_single(req->xfd, mrec->fmr_owner, 0, bulkstat);
	if (ret) {
		if (ret == ENOENT || ret == EINVAL)
			goto nothing_to_do;

		fprintf(stderr, _("bulkstat inode 0x%llx: %s\n"),
				(unsigned long long)mrec->fmr_owner,
				strerror(ret));
		return ret;
	}

	/*
	 * If we get stats for a different inode, the file may have been freed
	 * out from under us and there's nothing to do.
	 */
	if (bulkstat->bs_ino != mrec->fmr_owner)
		goto nothing_to_do;

	/*
	 * We're only allowed to open regular files and directories via handle
	 * so jump to online rebuild for all other file types.
	 */
	if (!S_ISREG(bulkstat->bs_mode) && !S_ISDIR(bulkstat->bs_mode))
		return csp_evac_file_metadata(req, target, mrec, -1,
				bulkstat);

	if (S_ISDIR(bulkstat->bs_mode))
		oflags = O_RDONLY;

	target_fd = csp_open_by_handle(req, oflags, mrec->fmr_owner,
			bulkstat->bs_gen);
	if (target_fd == -2)
		goto nothing_to_do;
	if (target_fd < 0)
		return -target_fd;

	/*
	 * Exchange only works for regular file data blocks.  If that isn't the
	 * case, our only recourse is online rebuild.
	 */
	if (S_ISDIR(bulkstat->bs_mode) ||
	    (mrec->fmr_flags & (FMR_OF_ATTR_FORK | FMR_OF_EXTENT_MAP))) {
		int	ret2;

		ret = csp_evac_file_metadata(req, target, mrec, target_fd,
				bulkstat);
		ret2 = close(target_fd);
		if (!ret && ret2)
			ret = ret2;
		return ret;
	}

	*fd = target_fd;
	return 0;

nothing_to_do:
	target->try_again = true;
	return 0;
}

/* Unshare the space in the work file that we're using for deduplication. */
static int
csp_unshare_workfile(
	struct clearspace_req	*req,
	unsigned long long	start,
	unsigned long long	length)
{
	int			ret;

	trace_falloc(req, "funshare workfd pos 0x%llx bytecount 0x%llx",
			start, length);

	ret = fallocate(req->work_fd, FALLOC_FL_UNSHARE_RANGE, start, length);
	if (ret) {
		perror(_("unsharing work file"));
		return ret;
	}

	ret = fsync(req->work_fd);
	if (ret) {
		perror(_("syncing work file"));
		return ret;
	}

	/* Make sure we didn't get any space within the clearing range. */
	start_bmapx_query(req, 0, start, length);
	while ((ret = run_bmapx_query(req, req->work_fd)) > 0) {
		struct getbmapx	*brec;

		for_each_bmapx_row(req, brec) {
			unsigned long long	p, l;

			trace_bmapx_rec(req, CSP_TRACE_FALLOC, brec);
			p = BBTOB(brec->bmv_block);
			l = BBTOB(brec->bmv_length);

			if (p + l < req->start || p >= req->start + req->length)
				continue;

			trace_prep(req,
	"workfd has extent inside clearing range, phys 0x%llx fsbcount 0x%llx",
					p, l);
			end_bmapx_query(req);
			return -1;
		}
	}
	end_bmapx_query(req);

	return 0;
}

/* Try to deduplicate every block in the fdr request, if we can. */
static int
csp_evac_dedupe_loop(
	struct clearspace_req		*req,
	struct clearspace_tgt		*target,
	unsigned long long		ino,
	int				max_reqlen)
{
	struct file_dedupe_range	*fdr = req->fdr;
	struct file_dedupe_range_info	*info = &fdr->info[0];
	loff_t				last_unshare_off = -1;
	int				ret;

	while (fdr->src_length > 0) {
		struct getbmapx		brec;
		bool			same;
		unsigned int		old_reqlen = fdr->src_length;

		if (max_reqlen && fdr->src_length > max_reqlen)
			fdr->src_length = max_reqlen;

		trace_dedupe(req, "ino 0x%llx pos 0x%llx bytecount 0x%llx",
				ino,
				(unsigned long long)info->dest_offset,
				(unsigned long long)fdr->src_length);

		ret = bmapx_one(req, req->work_fd, fdr->src_offset,
				fdr->src_length, &brec);
		if (ret)
			return ret;

		trace_dedupe(req, "workfd pos 0x%llx phys 0x%llx",
				(unsigned long long)fdr->src_offset,
				(unsigned long long)BBTOB(brec.bmv_block));

		ret = deduperange(req->work_fd, fdr, &same);
		if (ret == ENOSPC && last_unshare_off < fdr->src_offset) {
			req->trace_indent++;
			trace_dedupe(req, "funshare workfd at phys 0x%llx",
					(unsigned long long)fdr->src_offset);
			/*
			 * If we ran out of space, it's possible that we have
			 * reached the maximum sharing factor of the blocks in
			 * the work file.  Try unsharing the range of the work
			 * file to get a singly-owned range and loop again.
			 */
			ret = csp_unshare_workfile(req, fdr->src_offset,
					fdr->src_length);
			req->trace_indent--;
			if (ret)
				return ret;

			ret = fsync(req->work_fd);
			if (ret) {
				perror(_("sync after unshare work file"));
				return ret;
			}

			last_unshare_off = fdr->src_offset;
			fdr->src_length = old_reqlen;
			continue;
		}
		if (ret == EINVAL) {
			/*
			 * If we can't dedupe get the block, it's possible that
			 * src_fd was punched or truncated out from under us.
			 * Treat this the same way we would if the contents
			 * didn't match.
			 */
			trace_dedupe(req, "cannot evac space, moving on", 0);
			same = false;
			ret = 0;
		}
		if (ret) {
			fprintf(stderr, _("evacuating inode 0x%llx: %s\n"),
					ino, strerror(ret));
			return ret;
		}

		if (same) {
			req->trace_indent++;
			trace_dedupe(req,
	"evacuated ino 0x%llx pos 0x%llx bytecount 0x%llx",
					ino,
					(unsigned long long)info->dest_offset,
					(unsigned long long)info->bytes_deduped);
			req->trace_indent--;

			target->evacuated++;
		} else {
			req->trace_indent++;
			trace_dedupe(req,
	"failed evac ino 0x%llx pos 0x%llx bytecount 0x%llx",
					ino,
					(unsigned long long)info->dest_offset,
					(unsigned long long)fdr->src_length);
			req->trace_indent--;

			target->try_again = true;

			/*
			 * If we aren't single-stepping the deduplication,
			 * stop early so that the caller goes into single-step
			 * mode.
			 */
			if (!max_reqlen) {
				fdr->src_length = old_reqlen;
				return 0;
			}

			/* Contents changed, move on to the next block. */
			info->bytes_deduped = fdr->src_length;
		}
		fdr->src_length = old_reqlen;

		fdr->src_offset += info->bytes_deduped;
		info->dest_offset += info->bytes_deduped;
		fdr->src_length -= info->bytes_deduped;
	}

	return 0;
}

/*
 * Evacuate one fsmapping by using dedupe to remap data stored in the target
 * range to a copy stored in the work file.
 */
static int
csp_evac_dedupe_fsmap(
	struct clearspace_req		*req,
	struct clearspace_tgt		*target,
	const struct fsmap		*mrec)
{
	struct file_dedupe_range	*fdr = req->fdr;
	struct file_dedupe_range_info	*info = &fdr->info[0];
	bool				can_single_step;
	int				target_fd;
	int				ret, ret2;

	if (mrec->fmr_device != req->dev) {
		fprintf(stderr, _("wrong fsmap device in results.\n"));
		return -1;
	}

	ret = csp_evac_open(req, target, mrec, NULL, O_RDONLY, &target_fd);
	if (ret || target_fd < 0)
		return ret;

	/*
	 * Use dedupe to try to shift the target file's mappings to use the
	 * copy of the data that's in the work file.
	 */
	fdr->src_offset = mrec->fmr_physical;
	fdr->src_length = mrec->fmr_length;
	fdr->dest_count = 1;
	info->dest_fd = target_fd;
	info->dest_offset = mrec->fmr_offset;

	can_single_step = mrec->fmr_length > req->xfd->fsgeom.blocksize;

	/* First we try to do the entire thing all at once. */
	ret = csp_evac_dedupe_loop(req, target, mrec->fmr_owner, 0);
	if (ret)
		goto out_fd;

	/* If there's any work left, try again one block at a time. */
	if (can_single_step && fdr->src_length > 0) {
		ret = csp_evac_dedupe_loop(req, target, mrec->fmr_owner,
				req->xfd->fsgeom.blocksize);
		if (ret)
			goto out_fd;
	}

out_fd:
	ret2 = close(target_fd);
	if (!ret && ret2)
		ret = ret2;
	return ret;
}

/* Use deduplication to remap data extents away from where we're clearing. */
static int
csp_evac_dedupe(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target)
{
	int			ret;

	start_fsmap_query(req, req->dev, target->start, target->length);
	while ((ret = run_fsmap_query(req)) > 0) {
		struct fsmap	*mrec;

		for_each_fsmap_row(req, mrec) {
			trace_fsmap_rec(req, CSP_TRACE_DEDUPE, mrec);
			trim_target_fsmap(target, mrec);

			req->trace_indent++;
			ret = csp_evac_dedupe_fsmap(req, target, mrec);
			req->trace_indent--;
			if (ret)
				goto out;

			ret = csp_grab_free_space(req);
			if (ret)
				goto out;
		}
	}

out:
	end_fsmap_query(req);
	if (ret)
		trace_dedupe(req, "ret %d", ret);
	return ret;
}

#define BUFFERCOPY_BUFSZ		65536

/*
 * Use a memory buffer to copy part of src_fd to dst_fd, or return an errno. */
static int
csp_buffercopy(
	struct clearspace_req	*req,
	int			src_fd,
	loff_t			src_off,
	int			dst_fd,
	loff_t			dst_off,
	loff_t			len)
{
	int			ret = 0;

	while (len > 0) {
		size_t count = min(BUFFERCOPY_BUFSZ, len);
		ssize_t bytes_read, bytes_written;

		bytes_read = pread(src_fd, req->buf, count, src_off);
		if (bytes_read < 0) {
			ret = errno;
			break;
		}

		bytes_written = pwrite(dst_fd, req->buf, bytes_read, dst_off);
		if (bytes_written < 0) {
			ret = errno;
			break;
		}

		src_off += bytes_written;
		dst_off += bytes_written;
		len -= bytes_written;
	}

	return ret;
}

/*
 * Prepare the work file to assist in evacuating file data by copying the
 * contents of the frozen space into the work file.
 */
static int
csp_prepare_for_dedupe(
	struct clearspace_req	*req)
{
	struct file_clone_range	fcr;
	struct stat		statbuf;
	loff_t			datapos = 0;
	loff_t			length = 0;
	int			ret;

	ret = fstat(req->space_fd, &statbuf);
	if (ret) {
		perror(_("space capture file"));
		return ret;
	}

	ret = ftruncate(req->work_fd, 0);
	if (ret) {
		perror(_("truncate work file"));
		return ret;
	}

	ret = ftruncate(req->work_fd, statbuf.st_size);
	if (ret) {
		perror(_("reset work file"));
		return ret;
	}

	/* Make a working copy of the frozen file data. */
	start_spacefd_iter(req);
	while ((ret = spacefd_data_iter(req, &datapos, &length)) > 0) {
		trace_prep(req, "clone spacefd data 0x%llx length 0x%llx",
				(long long)datapos, (long long)length);

		fcr.src_fd = req->space_fd;
		fcr.src_offset = datapos;
		fcr.src_length = length;
		fcr.dest_offset = datapos;

		ret = clonerange(req->work_fd, &fcr);
		if (ret == ENOSPC) {
			req->trace_indent++;
			trace_prep(req,
	"falling back to buffered copy at 0x%llx",
					(long long)datapos);
			req->trace_indent--;
			ret = csp_buffercopy(req, req->space_fd, datapos,
					req->work_fd, datapos, length);
		}
		if (ret) {
			perror(
	_("copying space capture file contents to work file"));
			return ret;
		}
	}
	end_spacefd_iter(req);
	if (ret < 0)
		return ret;

	/*
	 * Unshare the work file so that it contains an identical copy of the
	 * contents of the space capture file but mapped to different blocks.
	 * This is key to using dedupe to migrate file space away from the
	 * requested region.
	 */
	req->trace_indent++;
	ret = csp_unshare_workfile(req, req->start, req->length);
	req->trace_indent--;
	return ret;
}

/*
 * Evacuate one fsmapping by using dedupe to remap data stored in the target
 * range to a copy stored in the work file.
 */
static int
csp_evac_exchange_fsmap(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target,
	const struct fsmap	*mrec)
{
	struct xfs_bulkstat	bulkstat;
	struct xfs_exch_range	xchg = { };
	struct getbmapx		brec;
	int			target_fd;
	int			ret, ret2;

	if (mrec->fmr_device != req->dev) {
		fprintf(stderr, _("wrong fsmap device in results.\n"));
		return -1;
	}

	ret = csp_evac_open(req, target, mrec, &bulkstat, O_RDWR, &target_fd);
	if (ret || target_fd < 0)
		return ret;

	ret = ftruncate(req->work_fd, 0);
	if (ret) {
		perror(_("truncating work file"));
		goto out_fd;
	}

	/*
	 * Copy the data from the original file to the work file.  We assume
	 * that the work file will end up with different data blocks and that
	 * they're outside of the requested range.
	 */
	ret = csp_buffercopy(req, target_fd, mrec->fmr_offset, req->work_fd,
			mrec->fmr_offset, mrec->fmr_length);
	if (ret) {
		fprintf(stderr, _("copying target file to work file: %s\n"),
				strerror(ret));
		goto out_fd;
	}

	ret = fsync(req->work_fd);
	if (ret) {
		perror(_("flush work file for fiexchange"));
		goto out_fd;
	}

	ret = bmapx_one(req, req->work_fd, mrec->fmr_physical,
			mrec->fmr_length, &brec);
	if (ret)
		return ret;

	trace_exchange(req, "workfd pos 0x%llx phys 0x%llx",
			(unsigned long long)mrec->fmr_physical,
			(unsigned long long)BBTOB(brec.bmv_block));

	/*
	 * Exchange the mappings, with the freshness check enabled.  This
	 * should result in the target file being switched to new blocks unless
	 * it has changed, in which case we bounce out and find a new target.
	 */
	xfrog_file_exchange_prep(NULL, XFS_EXCH_RANGE_NONATOMIC,
			mrec->fmr_offset, req->work_fd, mrec->fmr_offset,
			mrec->fmr_length, &xchg);
	xfrog_file_exchange_require_file2_fresh(&xchg, &bulkstat);
	ret = exchangerange(target_fd, &xchg);
	if (ret) {
		if (ret == EBUSY) {
			req->trace_indent++;
			trace_exchange(req,
 "failed evac ino 0x%llx pos 0x%llx bytecount 0x%llx",
					bulkstat.bs_ino,
					(unsigned long long)mrec->fmr_offset,
					(unsigned long long)mrec->fmr_length);
			req->trace_indent--;
			target->try_again = true;
		} else {
			fprintf(stderr,
	_("exchanging target and work file contents: %s\n"),
					strerror(ret));
		}
		goto out_fd;
	}

	req->trace_indent++;
	trace_exchange(req,
 "evacuated ino 0x%llx pos 0x%llx bytecount 0x%llx",
			bulkstat.bs_ino,
			(unsigned long long)mrec->fmr_offset,
			(unsigned long long)mrec->fmr_length);
	req->trace_indent--;
	target->evacuated++;

out_fd:
	ret2 = close(target_fd);
	if (!ret && ret2)
		ret = ret2;
	return ret;
}

/*
 * Try to evacuate all data blocks in the target region by copying the contents
 * to a new file and exchanging the extents.
 */
static int
csp_evac_exchange(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target)
{
	int			ret;

	start_fsmap_query(req, req->dev, target->start, target->length);
	while ((ret = run_fsmap_query(req)) > 0) {
		struct fsmap	*mrec;

		for_each_fsmap_row(req, mrec) {
			trace_fsmap_rec(req, CSP_TRACE_EXCHANGE, mrec);
			trim_target_fsmap(target, mrec);

			req->trace_indent++;
			ret = csp_evac_exchange_fsmap(req, target, mrec);
			req->trace_indent--;
			if (ret)
				goto out;

			ret = csp_grab_free_space(req);
			if (ret)
				goto out;
		}
	}
out:
	end_fsmap_query(req);
	if (ret)
		trace_exchange(req, "ret %d", ret);
	return ret;
}

/* Try to evacuate blocks by using online repair to rebuild AG metadata. */
static int
csp_evac_ag_metadata(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target,
	uint32_t		agno,
	uint32_t		mask)
{
	struct xfs_scrub_metadata scrub = {
		.sm_flags	= XFS_SCRUB_IFLAG_REPAIR |
				  XFS_SCRUB_IFLAG_FORCE_REBUILD,
	};
	unsigned int		i;
	int			ret;

	trace_xrebuild(req, "agno 0x%x mask 0x%x",
			(unsigned int)agno,
			(unsigned int)mask);

	for (i = XFS_SCRUB_TYPE_AGFL; i < XFS_SCRUB_TYPE_REFCNTBT; i++) {

		if (!(mask & (1U << i)))
			continue;

		scrub.sm_type = i;

		req->trace_indent++;
		trace_xrebuild(req, "agno %u type %u",
				(unsigned int)agno,
				(unsigned int)scrub.sm_type);
		req->trace_indent--;

		ret = ioctl(req->xfd->fd, XFS_IOC_SCRUB_METADATA, &scrub);
		if (ret) {
			if (errno == ENOENT || errno == ENOSPC)
				continue;
			fprintf(stderr, _("rebuilding ag %u type %u: %s\n"),
					(unsigned int)agno, scrub.sm_type,
					strerror(errno));
			return -1;
		}

		target->evacuated++;

		ret = csp_grab_free_space(req);
		if (ret)
			return ret;
	}

	return 0;
}

/* Compute a scrub mask for a fsmap special owner. */
static uint32_t
fsmap_owner_to_scrub_mask(__u64 owner)
{
	switch (owner) {
	case XFS_FMR_OWN_FREE:
	case XFS_FMR_OWN_UNKNOWN:
	case XFS_FMR_OWN_FS:
	case XFS_FMR_OWN_LOG:
		/* can't move these */
		return 0;
	case XFS_FMR_OWN_AG:
		return (1U << XFS_SCRUB_TYPE_BNOBT) |
		       (1U << XFS_SCRUB_TYPE_CNTBT) |
		       (1U << XFS_SCRUB_TYPE_AGFL) |
		       (1U << XFS_SCRUB_TYPE_RMAPBT);
	case XFS_FMR_OWN_INOBT:
		return (1U << XFS_SCRUB_TYPE_INOBT) |
		       (1U << XFS_SCRUB_TYPE_FINOBT);
	case XFS_FMR_OWN_REFC:
		return (1U << XFS_SCRUB_TYPE_REFCNTBT);
	case XFS_FMR_OWN_INODES:
	case XFS_FMR_OWN_COW:
		/* don't know how to get rid of these */
		return 0;
	case XFS_FMR_OWN_DEFECTIVE:
		/* good, get rid of it */
		return 0;
	default:
		return 0;
	}
}

/* Try to clear all per-AG metadata from the requested range. */
static int
csp_evac_fs_metadata(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target,
	bool			*cleared_anything)
{
	uint32_t		curr_agno = -1U;
	uint32_t		curr_mask = 0;
	int			ret = 0;

	if (req->realtime)
		return 0;

	start_fsmap_query(req, req->dev, target->start, target->length);
	while ((ret = run_fsmap_query(req)) > 0) {
		struct fsmap	*mrec;

		for_each_fsmap_row(req, mrec) {
			uint64_t	daddr;
			uint32_t	agno;
			uint32_t	mask;

			if (mrec->fmr_device != req->dev)
				continue;
			if (!(mrec->fmr_flags & FMR_OF_SPECIAL_OWNER))
				continue;

			/* Ignore regions that we already tried to clear. */
			if (bitmap_test(req->visited, mrec->fmr_physical,
						mrec->fmr_length))
				continue;

			mask = fsmap_owner_to_scrub_mask(mrec->fmr_owner);
			if (!mask)
				continue;

			trace_fsmap_rec(req, CSP_TRACE_XREBUILD, mrec);

			daddr = BTOBB(mrec->fmr_physical);
			agno = cvt_daddr_to_agno(req->xfd, daddr);

			trace_xrebuild(req,
	"agno 0x%x -> 0x%x mask 0x%x owner %lld",
					curr_agno, agno, curr_mask,
					(unsigned long long)mrec->fmr_owner);

			if (curr_agno == -1U) {
				curr_agno = agno;
			} else if (curr_agno != agno) {
				ret = csp_evac_ag_metadata(req, target,
						curr_agno, curr_mask);
				if (ret)
					goto out;

				*cleared_anything = true;
				curr_agno = agno;
				curr_mask = 0;
			}

			/* Put this on the list and try to clear it once. */
			curr_mask |= mask;
			ret = bitmap_set(req->visited, mrec->fmr_physical,
					mrec->fmr_length);
			if (ret) {
				perror(_("marking metadata extent visited"));
				goto out;
			}
		}
	}

	if (curr_agno != -1U && curr_mask != 0) {
		ret = csp_evac_ag_metadata(req, target, curr_agno, curr_mask);
		if (ret)
			goto out;
		*cleared_anything = true;
	}

	if (*cleared_anything)
		trace_bitmap(req, "set metadata start 0x%llx length 0x%llx",
				target->start, target->length);

out:
	end_fsmap_query(req);
	if (ret)
		trace_xrebuild(req, "ret %d", ret);
	return ret;
}

/*
 * Check that at least the start of the mapping was frozen into the work file
 * at the correct offset.  Set @len to the number of bytes that were frozen.
 * Returns -1 for error, zero if written extents are waiting to be mapped into
 * the space capture file, or 1 if there's nothing to transfer to the space
 * capture file.
 */
enum freeze_outcome {
	FREEZE_FAILED = -1,
	FREEZE_DONE,
	FREEZE_SKIP,
};

static enum freeze_outcome
csp_freeze_check_outcome(
	struct clearspace_req	*req,
	const struct fsmap	*mrec,
	unsigned long long	*len)
{
	struct getbmapx		brec;
	int			ret;

	*len = 0;

	ret = bmapx_one(req, req->work_fd, 0, mrec->fmr_length, &brec);
	if (ret)
		return FREEZE_FAILED;

	trace_freeze(req,
 "check if workfd pos 0x0 phys 0x%llx len 0x%llx maps to phys 0x%llx len 0x%llx",
			(unsigned long long)mrec->fmr_physical,
			(unsigned long long)mrec->fmr_length,
			(unsigned long long)BBTOB(brec.bmv_block),
			(unsigned long long)BBTOB(brec.bmv_length));

	/* freeze of an unwritten extent punches a hole in the work file. */
	if ((mrec->fmr_flags & FMR_OF_PREALLOC) && brec.bmv_block == -1) {
		*len = min(mrec->fmr_length, BBTOB(brec.bmv_length));
		return FREEZE_SKIP;
	}

	/*
	 * freeze of a written extent must result in the same physical space
	 * being mapped into the work file.
	 */
	if (!(mrec->fmr_flags & FMR_OF_PREALLOC) &&
	    BBTOB(brec.bmv_block) == mrec->fmr_physical) {
		*len = min(mrec->fmr_length, BBTOB(brec.bmv_length));
		return FREEZE_DONE;
	}

	/*
	 * We didn't find what we were looking for, which implies that the
	 * mapping changed out from under us.  Punch out everything that could
	 * have been mapped into the work file.  Set @len to zero and return so
	 * that we try again with the next mapping.
	 */
	trace_falloc(req, "reset workfd isize 0x0", 0);

	ret = ftruncate(req->work_fd, 0);
	if (ret) {
		perror(_("resetting work file after failed freeze"));
		return FREEZE_FAILED;
	}

	return FREEZE_SKIP;
}

/*
 * Open a file to try to freeze whatever data is in the requested range.
 *
 * Returns nonzero on error.  Returns zero and a file descriptor in @fd if the
 * caller is supposed to do something; or returns zero and @fd == -1 if there's
 * nothing to freeze.
 */
static int
csp_freeze_open(
	struct clearspace_req	*req,
	const struct fsmap	*mrec,
	int			*fd)
{
	struct xfs_bulkstat	bulkstat;
	int			oflags = O_RDWR;
	int			target_fd;
	int			ret;

	*fd = -1;

	ret = -xfrog_bulkstat_single(req->xfd, mrec->fmr_owner, 0, &bulkstat);
	if (ret) {
		if (ret == ENOENT || ret == EINVAL)
			return 0;

		fprintf(stderr, _("bulkstat inode 0x%llx: %s\n"),
				(unsigned long long)mrec->fmr_owner,
				strerror(errno));
		return ret;
	}

	/*
	 * If we get stats for a different inode, the file may have been freed
	 * out from under us and there's nothing to do.
	 */
	if (bulkstat.bs_ino != mrec->fmr_owner)
		return 0;

	/* Skip anything we can't freeze. */
	if (!S_ISREG(bulkstat.bs_mode) && !S_ISDIR(bulkstat.bs_mode))
		return 0;

	if (S_ISDIR(bulkstat.bs_mode))
		oflags = O_RDONLY;

	target_fd = csp_open_by_handle(req, oflags, mrec->fmr_owner,
			bulkstat.bs_gen);
	if (target_fd == -2)
		return 0;
	if (target_fd < 0)
		return target_fd;

	/*
	 * Skip mappings for directories, xattr data, and block mapping btree
	 * blocks.  We still have to close the file though.
	 */
	if (S_ISDIR(bulkstat.bs_mode) ||
	    (mrec->fmr_flags & (FMR_OF_ATTR_FORK | FMR_OF_EXTENT_MAP))) {
		return close(target_fd);
	}

	*fd = target_fd;
	return 0;
}

static inline uint64_t rounddown_64(uint64_t x, uint64_t y)
{
	return (x / y) * y;
}

/*
 * Deal with a frozen extent containing a partially written EOF block.  Either
 * we use funshare to get src_fd to release the block, or we reduce the length
 * of the frozen extent by one block.
 */
static int
csp_freeze_unaligned_eofblock(
	struct clearspace_req	*req,
	int			src_fd,
	const struct fsmap	*mrec,
	unsigned long long	*frozen_len)
{
	struct getbmapx		brec;
	struct stat		statbuf;
	loff_t			work_offset, length;
	int			ret;

	ret = fstat(req->work_fd, &statbuf);
	if (ret) {
		perror(_("statting work file"));
		return ret;
	}

	/*
	 * The frozen extent is less than the size of the work file, which
	 * means that we're already block aligned.
	 */
	if (*frozen_len <= statbuf.st_size)
		return 0;

	/* The frozen extent does not contain a partially written EOF block. */
	if (statbuf.st_size % statbuf.st_blksize == 0)
		return 0;

	/*
	 * Unshare what we think is a partially written EOF block of the
	 * original file, to try to force it to release that block.
	 */
	work_offset = rounddown_64(statbuf.st_size, statbuf.st_blksize);
	length = statbuf.st_size - work_offset;

	trace_freeze(req,
 "unaligned eofblock 0x%llx work_size 0x%llx blksize 0x%x work_offset 0x%llx work_length 0x%llx",
			*frozen_len, statbuf.st_size, statbuf.st_blksize,
			work_offset, length);

	ret = fallocate(src_fd, FALLOC_FL_UNSHARE_RANGE,
			mrec->fmr_offset + work_offset, length);
	if (ret) {
		perror(_("unsharing original file"));
		return ret;
	}

	ret = fsync(src_fd);
	if (ret) {
		perror(_("flushing original file"));
		return ret;
	}

	ret = bmapx_one(req, req->work_fd, work_offset, length, &brec);
	if (ret)
		return ret;

	if (BBTOB(brec.bmv_block) != mrec->fmr_physical + work_offset) {
		fprintf(stderr,
 _("work file offset 0x%llx maps to phys 0x%llx, expected 0x%llx"),
				(unsigned long long)work_offset,
				(unsigned long long)BBTOB(brec.bmv_block),
				(unsigned long long)mrec->fmr_physical);
		return -1;
	}

	/*
	 * If the block is still shared, there must be other owners of this
	 * block.  Round down the frozen length and we'll come back to it
	 * eventually.
	 */
	if (brec.bmv_oflags & BMV_OF_SHARED) {
		*frozen_len = work_offset;
		return 0;
	}

	/*
	 * Not shared anymore, so increase the size of the file to the next
	 * block boundary so that we can reflink it into the space capture
	 * file.
	 */
	ret = ftruncate(req->work_fd,
			BBTOB(brec.bmv_length) + BBTOB(brec.bmv_offset));
	if (ret) {
		perror(_("expanding work file"));
		return ret;
	}

	/* Double-check that we didn't lose the block. */
	ret = bmapx_one(req, req->work_fd, work_offset, length, &brec);
	if (ret)
		return ret;

	if (BBTOB(brec.bmv_block) != mrec->fmr_physical + work_offset) {
		fprintf(stderr,
 _("work file offset 0x%llx maps to phys 0x%llx, should be 0x%llx"),
				(unsigned long long)work_offset,
				(unsigned long long)BBTOB(brec.bmv_block),
				(unsigned long long)mrec->fmr_physical);
		return -1;
	}

	return 0;
}

/*
 * Given a fsmap, try to reflink the physical space into the space capture
 * file.
 */
static int
csp_freeze_req_fsmap(
	struct clearspace_req	*req,
	unsigned long long	*cursor,
	const struct fsmap	*mrec)
{
	struct fsmap		short_mrec;
	struct file_clone_range	fcr = { };
	unsigned long long	frozen_len;
	enum freeze_outcome	outcome;
	int			src_fd;
	int			ret, ret2;

	if (mrec->fmr_device != req->dev) {
		fprintf(stderr, _("wrong fsmap device in results.\n"));
		return -1;
	}

	/* Ignore mappings for our secret files. */
	if (csp_is_internal_owner(req, mrec->fmr_owner))
		return 0;

	/* Ignore mappings before the cursor. */
	if (mrec->fmr_physical + mrec->fmr_length < *cursor)
		return 0;

	/* Jump past mappings for metadata. */
	if (mrec->fmr_flags & FMR_OF_SPECIAL_OWNER)
		goto skip;

	/*
	 * Open this file so that we can try to freeze its data blocks.
	 * For other types of files we just skip to the evacuation step.
	 */
	ret = csp_freeze_open(req, mrec, &src_fd);
	if (ret)
		return ret;
	if (src_fd < 0)
		goto skip;

	/*
	 * If the cursor is in the middle of this mapping, increase the start
	 * of the mapping to start at the cursor.
	 */
	if (mrec->fmr_physical < *cursor) {
		unsigned long long	delta = *cursor - mrec->fmr_physical;

		short_mrec = *mrec;
		short_mrec.fmr_physical = *cursor;
		short_mrec.fmr_offset += delta;
		short_mrec.fmr_length -= delta;

		mrec = &short_mrec;
	}

	req->trace_indent++;
	if (mrec->fmr_length == 0) {
		trace_freeze(req, "skipping zero-length freeze", 0);
		goto out_fd;
	}

	/*
	 * Reflink the mapping from the source file into the empty work file so
	 * that a write will be written elsewhere.  The only way to reflink a
	 * partially written EOF block is if the kernel can reset the work file
	 * size so that the post-EOF part of the block remains post-EOF.  If we
	 * can't do that, we're sunk.  If the mapping is unwritten, we'll leave
	 * a hole in the work file.
	 */
	ret = ftruncate(req->work_fd, 0);
	if (ret) {
		perror(_("truncating work file for freeze"));
		goto out_fd;
	}

	fcr.src_fd = src_fd;
	fcr.src_offset = mrec->fmr_offset;
	fcr.src_length = mrec->fmr_length;
	fcr.dest_offset = 0;

	trace_freeze(req,
 "reflink ino 0x%llx offset 0x%llx bytecount 0x%llx into workfd",
			(unsigned long long)mrec->fmr_owner,
			(unsigned long long)fcr.src_offset,
			(unsigned long long)fcr.src_length);

	ret = clonerange(req->work_fd, &fcr);
	if (ret == EINVAL) {
		/*
		 * If that didn't work, try reflinking to EOF and picking out
		 * whatever pieces we want.
		 */
		fcr.src_length = 0;

		trace_freeze(req,
 "reflink ino 0x%llx offset 0x%llx to EOF into workfd",
				(unsigned long long)mrec->fmr_owner,
				(unsigned long long)fcr.src_offset);

		ret = clonerange(req->work_fd, &fcr);
	}
	if (ret == EINVAL) {
		/*
		 * If we still can't get the block, it's possible that src_fd
		 * was punched or truncated out from under us, so we just move
		 * on to the next fsmap.
		 */
		trace_freeze(req, "cannot freeze space, moving on", 0);
		ret = 0;
		goto out_fd;
	}
	if (ret) {
		fprintf(stderr, _("freezing space to work file: %s\n"),
				strerror(ret));
		goto out_fd;
	}

	req->trace_indent++;
	outcome = csp_freeze_check_outcome(req, mrec, &frozen_len);
	req->trace_indent--;
	switch (outcome) {
	case FREEZE_FAILED:
		ret = -1;
		goto out_fd;
	case FREEZE_SKIP:
		*cursor += frozen_len;
		goto out_fd;
	case FREEZE_DONE:
		break;
	}

	/*
	 * If we tried reflinking to EOF to capture a partially written EOF
	 * block in the work file, we need to unshare the end of the source
	 * file before we try to reflink the frozen space into the space
	 * capture file.
	 */
	if (fcr.src_length == 0) {
		ret = csp_freeze_unaligned_eofblock(req, src_fd, mrec,
				&frozen_len);
		if (ret)
			goto out_fd;
	}

	/*
	 * We've frozen the mapping by reflinking it into the work file and
	 * confirmed that the work file has the space we wanted.  Now we need
	 * to map the same extent into the space capture file.  If reflink
	 * fails because we're out of space, fall back to EXCHANGE_RANGE.  The
	 * end goal is to populate the space capture file; we don't care about
	 * the contents of the work file.
	 */
	fcr.src_fd = req->work_fd;
	fcr.src_offset = 0;
	fcr.dest_offset = mrec->fmr_physical;
	fcr.src_length = frozen_len;

	trace_freeze(req, "reflink phys 0x%llx len 0x%llx to spacefd",
			(unsigned long long)mrec->fmr_physical,
			(unsigned long long)mrec->fmr_length);

	ret = clonerange(req->space_fd, &fcr);
	if (ret == ENOSPC) {
		struct xfs_exch_range	xchg;

		xfrog_file_exchange_prep(NULL, XFS_EXCH_RANGE_NONATOMIC,
				mrec->fmr_physical, req->work_fd,
				mrec->fmr_physical, frozen_len, &xchg);
		ret = exchangerange(req->space_fd, &xchg);
	}
	if (ret) {
		fprintf(stderr, _("freezing space to space capture file: %s\n"),
				strerror(ret));
		goto out_fd;
	}

	*cursor += frozen_len;
out_fd:
	ret2 = close(src_fd);
	if (!ret && ret2)
		ret = ret2;
	req->trace_indent--;
	if (ret)
		trace_freeze(req, "ret %d", ret);
	return ret;
skip:
	*cursor += mrec->fmr_length;
	return 0;
}

/*
 * Try to freeze all the space in the requested range against overwrites.
 *
 * For each file data fsmap within each hole in the part of the space capture
 * file corresponding to the requested range, try to reflink the space into the
 * space capture file so that any subsequent writes to the original owner are
 * CoW and nobody else can allocate the space.  If we cannot use reflink to
 * freeze all the space, we cannot proceed with the clearing.
 */
static int
csp_freeze_req_range(
	struct clearspace_req	*req)
{
	unsigned long long	cursor = req->start;
	loff_t			holepos = 0;
	loff_t			length = 0;
	int			ret;

	ret = ftruncate(req->space_fd, req->start + req->length);
	if (ret) {
		perror(_("setting up space capture file"));
		return ret;
	}

	if (!req->use_reflink)
		return 0;

	start_spacefd_iter(req);
	while ((ret = spacefd_hole_iter(req, &holepos, &length)) > 0) {
		trace_freeze(req, "spacefd hole 0x%llx length 0x%llx",
				(long long)holepos, (long long)length);

		start_fsmap_query(req, req->dev, holepos, length);
		while ((ret = run_fsmap_query(req)) > 0) {
			struct fsmap	*mrec;

			for_each_fsmap_row(req, mrec) {
				trace_fsmap_rec(req, CSP_TRACE_FREEZE, mrec);
				trim_request_fsmap(req, mrec);
				ret = csp_freeze_req_fsmap(req, &cursor, mrec);
				if (ret) {
					end_fsmap_query(req);
					goto out;
				}
			}
		}
		end_fsmap_query(req);
	}
out:
	end_spacefd_iter(req);
	return ret;
}

/*
 * Dump all speculative preallocations, COW staging blocks, and inactive inodes
 * to try to free up as much space as we can.
 */
static int
csp_collect_garbage(
	struct clearspace_req	*req)
{
	struct xfs_fs_eofblocks	eofb = {
		.eof_version	= XFS_EOFBLOCKS_VERSION,
		.eof_flags	= XFS_EOF_FLAGS_SYNC,
	};
	int			ret;

	ret = ioctl(req->xfd->fd, XFS_IOC_FREE_EOFBLOCKS, &eofb);
	if (ret) {
		perror(_("xfs garbage collector"));
		return -1;
	}

	return 0;
}

static int
csp_prepare(
	struct clearspace_req	*req)
{
	blkcnt_t		old_blocks = 0;
	int			ret;

	/*
	 * Empty out CoW forks and speculative post-EOF preallocations before
	 * starting the clearing process.  This may be somewhat overkill.
	 */
	ret = syncfs(req->xfd->fd);
	if (ret) {
		perror(_("syncing filesystem"));
		return ret;
	}

	ret = csp_collect_garbage(req);
	if (ret)
		return ret;

	/*
	 * Set up the space capture file as a large sparse file mirroring the
	 * physical space that we want to defragment.
	 */
	ret = ftruncate(req->space_fd, req->start + req->length);
	if (ret) {
		perror(_("setting up space capture file"));
		return ret;
	}

	/*
	 * If we don't have reflink, just grab the free space and move on to
	 * copying and exchanging file contents.
	 */
	if (!req->use_reflink)
		return csp_grab_free_space(req);

	/*
	 * Try to freeze as much of the requested range as we can, grab the
	 * free space in that range, and run freeze again to pick up anything
	 * that may have been allocated while all that was going on.
	 */
	do {
		struct stat	statbuf;

		ret = csp_freeze_req_range(req);
		if (ret)
			return ret;

		ret = csp_grab_free_space(req);
		if (ret)
			return ret;

		ret = fstat(req->space_fd, &statbuf);
		if (ret)
			return ret;

		if (old_blocks == statbuf.st_blocks)
			break;
		old_blocks = statbuf.st_blocks;
	} while (1);

	/*
	 * If reflink is enabled, our strategy is to dedupe to free blocks in
	 * the area that we're clearing without making any user-visible changes
	 * to the file contents.  For all the written file data blocks in area
	 * we're clearing, make an identical copy in the work file that is
	 * backed by blocks that are not in the clearing area.
	 */
	return csp_prepare_for_dedupe(req);
}

/* Set up the target to clear all metadata from the given range. */
static inline void
csp_target_metadata(
	struct clearspace_req	*req,
	struct clearspace_tgt	*target)
{
	target->start = req->start;
	target->length = req->length;
	target->prio = 0;
	target->evacuated = 0;
	target->owners = 0;
	target->try_again = false;
}

/*
 * Loop through the space to find the most appealing part of the device to
 * clear, then try to evacuate everything within.
 */
int
clearspace_run(
	struct clearspace_req	*req)
{
	struct clearspace_tgt	target;
	const struct csp_errstr	*es;
	bool			cleared_anything;
	int			ret;

	if (req->trace_mask) {
		fprintf(stderr, "debug flags 0x%x:", req->trace_mask);
		for (es = errtags; es->tag; es++) {
			if (req->trace_mask & es->mask)
				fprintf(stderr, " %s", es->tag);
		}
		fprintf(stderr, "\n");
	}

	req->trace_indent = 0;
	trace_status(req,
 _("Clearing dev %u:%u physical 0x%llx bytecount 0x%llx."),
			major(req->dev), minor(req->dev),
			req->start, req->length);

	if (req->trace_mask & ~CSP_TRACE_STATUS)
		trace_status(req, "reflink? %d evac_metadata? %d",
				req->use_reflink, req->can_evac_metadata);

	ret = bitmap_alloc(&req->visited);
	if (ret) {
		perror(_("allocating visited bitmap"));
		return ret;
	}

	ret = csp_prepare(req);
	if (ret)
		goto out_bitmap;

	/* Evacuate as many file blocks as we can. */
	do {
		ret = csp_find_target(req, &target);
		if (ret)
			goto out_bitmap;

		if (target.length == 0)
			break;

		trace_target(req,
	"phys 0x%llx len 0x%llx owners 0x%llx prio 0x%llx",
				target.start, target.length,
				target.owners, target.prio);

		if (req->use_reflink)
			ret = csp_evac_dedupe(req, &target);
		else
			ret = csp_evac_exchange(req, &target);
		if (ret)
			goto out_bitmap;

		trace_status(req, _("Evacuated %llu file items."),
				target.evacuated);
	} while (target.evacuated > 0 || target.try_again);

	if (!req->can_evac_metadata)
		goto out_bitmap;

	/* Evacuate as many AG metadata blocks as we can. */
	do {
		csp_target_metadata(req, &target);

		ret = csp_evac_fs_metadata(req, &target, &cleared_anything);
		if (ret)
			goto out_bitmap;

		trace_status(req, "evacuated %llu metadata items",
				target.evacuated);
	} while (target.evacuated > 0 && cleared_anything);

out_bitmap:
	bitmap_free(&req->visited);
	return ret;
}

/* How much space did we actually clear? */
int
clearspace_efficacy(
	struct clearspace_req	*req,
	unsigned long long	*cleared_bytes)
{
	unsigned long long	cleared = 0;
	int			ret;

	start_bmapx_query(req, 0, req->start, req->length);
	while ((ret = run_bmapx_query(req, req->space_fd)) > 0) {
		struct getbmapx	*brec;

		for_each_bmapx_row(req, brec) {
			if (brec->bmv_block == -1)
				continue;

			trace_bmapx_rec(req, CSP_TRACE_EFFICACY, brec);

			if (brec->bmv_offset != brec->bmv_block) {
				fprintf(stderr,
	_("space capture file mapped incorrectly\n"));
				end_bmapx_query(req);
				return -1;
			}
			cleared += BBTOB(brec->bmv_length);
		}
	}
	end_bmapx_query(req);
	if (ret)
		return ret;

	*cleared_bytes = cleared;
	return 0;
}

/*
 * Create a temporary file on the same volume (data/rt) that we're trying to
 * clear free space on.
 */
static int
csp_open_tempfile(
	struct clearspace_req	*req,
	struct stat		*statbuf)
{
	struct fsxattr		fsx;
	int			fd, ret;

	fd = openat(req->xfd->fd, ".", O_TMPFILE | O_RDWR | O_EXCL, 0600);
	if (fd < 0) {
		perror(_("opening temp file"));
		return -1;
	}

	/* Make sure we got the same filesystem as the open file. */
	ret = fstat(fd, statbuf);
	if (ret) {
		perror(_("stat temp file"));
		goto fail;
	}
	if (statbuf->st_dev != req->statbuf.st_dev) {
		fprintf(stderr,
	_("Cannot create temp file on same fs as open file.\n"));
		goto fail;
	}

	/* Ensure this file targets the correct data/rt device. */
	ret = ioctl(fd, FS_IOC_FSGETXATTR, &fsx);
	if (ret) {
		perror(_("FSGETXATTR temp file"));
		goto fail;
	}

	if (!!(fsx.fsx_xflags & FS_XFLAG_REALTIME) != req->realtime) {
		if (req->realtime)
			fsx.fsx_xflags |= FS_XFLAG_REALTIME;
		else
			fsx.fsx_xflags &= ~FS_XFLAG_REALTIME;

		ret = ioctl(fd, FS_IOC_FSSETXATTR, &fsx);
		if (ret) {
			perror(_("FSSETXATTR temp file"));
			goto fail;
		}
	}

	trace_setup(req, "opening temp inode 0x%llx as fd %d",
			(unsigned long long)statbuf->st_ino, fd);

	return fd;
fail:
	close(fd);
	return -1;
}

/* Extract fshandle from the open file. */
static int
csp_install_file(
	struct clearspace_req	*req,
	struct xfs_fd		*xfd)
{
	void			*handle;
	size_t			handle_sz;
	int			ret;

	ret = fstat(xfd->fd, &req->statbuf);
	if (ret)
		return ret;

	if (!S_ISDIR(req->statbuf.st_mode)) {
		errno = -ENOTDIR;
		return -1;
	}

	ret = fd_to_handle(xfd->fd, &handle, &handle_sz);
	if (ret)
		return ret;

	ret = handle_to_fshandle(handle, handle_sz, &req->fshandle,
			&req->fshandle_sz);
	if (ret)
		return ret;

	free_handle(handle, handle_sz);
	req->xfd = xfd;
	return 0;
}

/* Decide if we can use online repair to evacuate metadata. */
static void
csp_detect_evac_metadata(
	struct clearspace_req		*req)
{
	struct xfs_scrub_metadata	scrub = {
		.sm_type		= XFS_SCRUB_TYPE_PROBE,
		.sm_flags		= XFS_SCRUB_IFLAG_REPAIR |
					  XFS_SCRUB_IFLAG_FORCE_REBUILD,
	};
	int				ret;

	ret = ioctl(req->xfd->fd, XFS_IOC_SCRUB_METADATA, &scrub);
	if (ret)
		return;

	/*
	 * We'll try to evacuate metadata if the probe works.  This doesn't
	 * guarantee success; it merely means that the kernel call exists.
	 */
	req->can_evac_metadata = true;
}

/* Detect XFS_IOC_MAP_FREESP; this is critical for grabbing free space! */
static int
csp_detect_map_freesp(
	struct clearspace_req	*req)
{
	struct xfs_map_freesp	args = {
		.offset		= 0,
		.len		= 1,
	};
	int			ret;

	/*
	 * A single-byte fallocate request will succeed without doing anything
	 * to the filesystem.
	 */
	ret = ioctl(req->work_fd, XFS_IOC_MAP_FREESP, &args);
	if (!ret)
		return 0;

	if (errno == EOPNOTSUPP) {
		fprintf(stderr,
	_("Filesystem does not support XFS_IOC_MAP_FREESP\n"));
		return -1;
	}

	perror(_("test XFS_IOC_MAP_FREESP on work file"));
	return -1;
}

/*
 * Assemble operation information to clear the physical space in part of a
 * filesystem.
 */
int
clearspace_init(
	struct clearspace_req		**reqp,
	const struct clearspace_init	*attrs)
{
	struct clearspace_req		*req;
	int				ret;

	req = calloc(1, sizeof(struct clearspace_req));
	if (!req) {
		perror(_("malloc clearspace"));
		return -1;
	}

	req->work_fd = -1;
	req->space_fd = -1;
	req->trace_mask = attrs->trace_mask;

	req->realtime = attrs->is_realtime;
	req->dev = attrs->dev;
	req->start = attrs->start;
	req->length = attrs->length;

	ret = csp_install_file(req, attrs->xfd);
	if (ret) {
		perror(attrs->fname);
		goto fail;
	}

	csp_detect_evac_metadata(req);

	req->work_fd = csp_open_tempfile(req, &req->temp_statbuf);
	if (req->work_fd < 0)
		goto fail;

	req->space_fd = csp_open_tempfile(req, &req->space_statbuf);
	if (req->space_fd < 0)
		goto fail;

	ret = csp_detect_map_freesp(req);
	if (ret)
		goto fail;

	req->mhead = calloc(1, fsmap_sizeof(QUERY_BATCH_SIZE));
	if (!req->mhead) {
		perror(_("opening fs mapping query"));
		goto fail;
	}

	req->rhead = calloc(1, xfs_getfsrefs_sizeof(QUERY_BATCH_SIZE));
	if (!req->rhead) {
		perror(_("opening refcount query"));
		goto fail;
	}

	req->bhead = calloc(QUERY_BATCH_SIZE + 1, sizeof(struct getbmapx));
	if (!req->bhead) {
		perror(_("opening file mapping query"));
		goto fail;
	}

	req->buf = malloc(BUFFERCOPY_BUFSZ);
	if (!req->buf) {
		perror(_("allocating file copy buffer"));
		goto fail;
	}

	req->fdr = calloc(1, sizeof(struct file_dedupe_range) +
			     sizeof(struct file_dedupe_range_info));
	if (!req->fdr) {
		perror(_("allocating dedupe control buffer"));
		goto fail;
	}

	req->use_reflink = req->xfd->fsgeom.flags & XFS_FSOP_GEOM_FLAGS_REFLINK;

	*reqp = req;
	return 0;
fail:
	clearspace_free(&req);
	return -1;
}

/* Free all resources associated with a space clearing request. */
int
clearspace_free(
	struct clearspace_req	**reqp)
{
	struct clearspace_req	*req = *reqp;
	int			ret = 0;

	if (!req)
		return 0;

	*reqp = NULL;
	free(req->fdr);
	free(req->buf);
	free(req->bhead);
	free(req->rhead);
	free(req->mhead);

	if (req->space_fd >= 0) {
		ret = close(req->space_fd);
		if (ret)
			perror(_("closing space capture file"));
	}

	if (req->work_fd >= 0) {
		int	ret2 = close(req->work_fd);

		if (ret2) {
			perror(_("closing work file"));
			if (!ret && ret2)
				ret = ret2;
		}
	}

	if (req->fshandle)
		free_handle(req->fshandle, req->fshandle_sz);
	free(req);
	return ret;
}
