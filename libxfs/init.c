// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include <sys/stat.h>
#include "init.h"

#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount_btree.h"
#include "libfrog/platform.h"

#include "libxfs.h"		/* for now */

#ifndef HAVE_LIBURCU_ATOMIC64
pthread_mutex_t	atomic64_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

char *progname = "libxfs";	/* default, changed by each tool */

struct cache *libxfs_bcache;	/* global buffer cache */
int libxfs_bhash_size;		/* #buckets in bcache */

int	use_xfs_buf_lock;	/* global flag: use xfs_buf locks for MT */

static int nextfakedev = -1;	/* device number to give to next fake device */

/*
 * Checks whether a given device has a mounted, writable
 * filesystem, returns 1 if it does & fatal (just warns
 * if not fatal, but allows us to proceed).
 *
 * Useful to tools which will produce uncertain results
 * if the filesystem is active - repair, check, logprint.
 */
static int
check_isactive(char *name, char *block, int fatal)
{
	struct stat	st;

	if (stat(block, &st) < 0)
		return 0;
	if ((st.st_mode & S_IFMT) != S_IFBLK)
		return 0;
	if (platform_check_ismounted(name, block, &st, 0) == 0)
		return 0;
	if (platform_check_iswritable(name, block, &st))
		return fatal ? 1 : 0;
	return 0;
}

static int
check_open(
	struct libxfs_init	*xi,
	struct libxfs_dev	*dev)
{
	struct stat	stbuf;

	if (stat(dev->name, &stbuf) < 0) {
		perror(dev->name);
		return 0;
	}
	if (!(xi->flags & LIBXFS_ISREADONLY) &&
	    !(xi->flags & LIBXFS_ISINACTIVE) &&
	    platform_check_ismounted(dev->name, dev->name, NULL, 1))
		return 0;

	if ((xi->flags & LIBXFS_ISINACTIVE) &&
	    check_isactive(dev->name, dev->name, !!(xi->flags &
			(LIBXFS_ISREADONLY | LIBXFS_DANGEROUSLY))))
		return 0;

	return 1;
}

static bool
libxfs_device_open(
	struct libxfs_init	*xi,
	struct libxfs_dev	*dev)
{
	struct stat		statb;
	int			flags;

	dev->fd = -1;

	if (!dev->name)
		return true;
	if (!dev->isfile && !check_open(xi, dev))
		return false;

	if (xi->flags & LIBXFS_ISREADONLY)
		flags = O_RDONLY;
	else
		flags = O_RDWR;

	if (dev->create) {
		flags |= O_CREAT | O_TRUNC;
	} else {
		if (xi->flags & LIBXFS_EXCLUSIVELY)
			flags |= O_EXCL;
		if ((xi->flags & LIBXFS_DIRECT) && platform_direct_blockdev())
			flags |= O_DIRECT;
	}

retry:
	dev->fd = open(dev->name, flags, 0666);
	if (dev->fd < 0) {
		if (errno == EINVAL && (flags & O_DIRECT)) {
			flags &= ~O_DIRECT;
			goto retry;
		}
		fprintf(stderr, _("%s: cannot open %s: %s\n"),
			progname, dev->name, strerror(errno));
		exit(1);
	}

	if (fstat(dev->fd, &statb) < 0) {
		fprintf(stderr, _("%s: cannot stat %s: %s\n"),
			progname, dev->name, strerror(errno));
		exit(1);
	}

	if (!(xi->flags & LIBXFS_ISREADONLY) &&
	    xi->setblksize &&
	    (statb.st_mode & S_IFMT) == S_IFBLK) {
		/*
		 * Try to use the given explicit blocksize.  Failure to set the
		 * block size is only fatal for direct I/O.
		 */
		platform_set_blocksize(dev->fd, dev->name, statb.st_rdev,
				xi->setblksize, flags & O_DIRECT);
	}

	/*
	 * Get the device number from the stat buf - unless we're not opening a
	 * real device, in which case choose a new fake device number.
	 */
	if (statb.st_rdev)
		dev->dev = statb.st_rdev;
	else
		dev->dev = nextfakedev--;
	platform_findsizes(dev->name, dev->fd, &dev->size, &dev->bsize);
	return true;
}

static void
libxfs_device_close(
	struct libxfs_dev	*dev)
{
	int			ret;

	ret = platform_flush_device(dev->fd, dev->dev);
	if (ret) {
		ret = -errno;
		fprintf(stderr,
	_("%s: flush of device %lld failed, err=%d"),
			progname, (long long)dev, ret);
	}
	close(dev->fd);

	dev->fd = -1;
	dev->dev = 0;
}

/*
 * Initialize/destroy all of the cache allocators we use.
 */
static void
init_caches(void)
{
	int	error;

	/* initialise cache allocation */
	xfs_buf_cache = kmem_cache_init(sizeof(struct xfs_buf), "xfs_buffer");
	xfs_inode_cache = kmem_cache_init(sizeof(struct xfs_inode), "xfs_inode");
	xfs_ifork_cache = kmem_cache_init(sizeof(struct xfs_ifork), "xfs_ifork");
	xfs_ili_cache = kmem_cache_init(
			sizeof(struct xfs_inode_log_item),"xfs_inode_log_item");
	xfs_buf_item_cache = kmem_cache_init(
			sizeof(struct xfs_buf_log_item), "xfs_buf_log_item");
	error = xfs_defer_init_item_caches();
	if (error) {
		fprintf(stderr, "Could not allocate defer init item caches.\n");
		abort();
	}
	xfs_da_state_cache = kmem_cache_init(
			sizeof(struct xfs_da_state), "xfs_da_state");
	error = xfs_btree_init_cur_caches();
	if (error) {
		fprintf(stderr, "Could not allocate btree cursor caches.\n");
		abort();
	}
	xfs_extfree_item_cache = kmem_cache_init(
			sizeof(struct xfs_extent_free_item),
			"xfs_extfree_item");
	xfs_trans_cache = kmem_cache_init(
			sizeof(struct xfs_trans), "xfs_trans");
}

static int
destroy_caches(void)
{
	int	leaked = 0;

	leaked += kmem_cache_destroy(xfs_buf_cache);
	leaked += kmem_cache_destroy(xfs_ili_cache);
	leaked += kmem_cache_destroy(xfs_inode_cache);
	leaked += kmem_cache_destroy(xfs_ifork_cache);
	leaked += kmem_cache_destroy(xfs_buf_item_cache);
	leaked += kmem_cache_destroy(xfs_da_state_cache);
	xfs_defer_destroy_item_caches();
	xfs_btree_destroy_cur_caches();
	leaked += kmem_cache_destroy(xfs_extfree_item_cache);
	leaked += kmem_cache_destroy(xfs_trans_cache);

	return leaked;
}

static void
libxfs_close_devices(
	struct libxfs_init	*li)
{
	if (li->data.dev)
		libxfs_device_close(&li->data);
	if (li->log.dev && li->log.dev != li->data.dev)
		libxfs_device_close(&li->log);
	if (li->rt.dev)
		libxfs_device_close(&li->rt);
}

/*
 * libxfs initialization.
 * Caller gets a 0 on failure (and we print a message), 1 on success.
 */
int
libxfs_init(struct libxfs_init *a)
{
	rcu_init();
	rcu_register_thread();
	radix_tree_init();

	if (!libxfs_device_open(a, &a->data))
		goto done;
	if (!libxfs_device_open(a, &a->log))
		goto done;
	if (!libxfs_device_open(a, &a->rt))
		goto done;

	if (!libxfs_bhash_size)
		libxfs_bhash_size = LIBXFS_BHASHSIZE(sbp);
	libxfs_bcache = cache_init(a->bcache_flags, libxfs_bhash_size,
				   &libxfs_bcache_operations);
	use_xfs_buf_lock = a->flags & LIBXFS_USEBUFLOCK;
	xfs_dir_startup();
	init_caches();
	return 1;

done:
	libxfs_close_devices(a);
	rcu_unregister_thread();
	return 0;
}


/*
 * Initialize realtime fields in the mount structure.
 */
static int
rtmount_init(
	xfs_mount_t	*mp)	/* file system mount structure */
{
	struct xfs_buf	*bp;	/* buffer for last block of subvolume */
	xfs_daddr_t	d;	/* address of last block of subvolume */
	int		error;

	if (mp->m_sb.sb_rblocks == 0)
		return 0;

	if (xfs_has_reflink(mp)) {
		fprintf(stderr,
	_("%s: Reflink not compatible with realtime device. Please try a newer xfsprogs.\n"),
				progname);
		return -1;
	}

	if (xfs_has_rmapbt(mp)) {
		fprintf(stderr,
	_("%s: Reverse mapping btree not compatible with realtime device. Please try a newer xfsprogs.\n"),
				progname);
		return -1;
	}

	if (mp->m_rtdev_targp->bt_bdev == 0 && !xfs_is_debugger(mp)) {
		fprintf(stderr, _("%s: filesystem has a realtime subvolume\n"),
			progname);
		return -1;
	}
	mp->m_rsumlevels = mp->m_sb.sb_rextslog + 1;
	mp->m_rsumsize =
		(uint)sizeof(xfs_suminfo_t) * mp->m_rsumlevels *
		mp->m_sb.sb_rbmblocks;
	mp->m_rsumsize = roundup(mp->m_rsumsize, mp->m_sb.sb_blocksize);
	mp->m_rbmip = mp->m_rsumip = NULL;

	/*
	 * Allow debugger to be run without the realtime device present.
	 */
	if (xfs_is_debugger(mp))
		return 0;

	/*
	 * Check that the realtime section is an ok size.
	 */
	d = (xfs_daddr_t)XFS_FSB_TO_BB(mp, mp->m_sb.sb_rblocks);
	if (XFS_BB_TO_FSB(mp, d) != mp->m_sb.sb_rblocks) {
		fprintf(stderr, _("%s: realtime init - %llu != %llu\n"),
			progname, (unsigned long long) XFS_BB_TO_FSB(mp, d),
			(unsigned long long) mp->m_sb.sb_rblocks);
		return -1;
	}
	error = libxfs_buf_read(mp->m_rtdev, d - XFS_FSB_TO_BB(mp, 1),
			XFS_FSB_TO_BB(mp, 1), 0, &bp, NULL);
	if (error) {
		fprintf(stderr, _("%s: realtime size check failed\n"),
			progname);
		return -1;
	}
	libxfs_buf_relse(bp);
	return 0;
}

static bool
xfs_set_inode_alloc_perag(
	struct xfs_perag	*pag,
	xfs_ino_t		ino,
	xfs_agnumber_t		max_metadata)
{
	if (!xfs_is_inode32(pag->pag_mount)) {
		set_bit(XFS_AGSTATE_ALLOWS_INODES, &pag->pag_opstate);
		clear_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
		return false;
	}

	if (ino > XFS_MAXINUMBER_32) {
		clear_bit(XFS_AGSTATE_ALLOWS_INODES, &pag->pag_opstate);
		clear_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
		return false;
	}

	set_bit(XFS_AGSTATE_ALLOWS_INODES, &pag->pag_opstate);
	if (pag->pag_agno < max_metadata)
		set_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
	else
		clear_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
	return true;
}

/*
 * Set parameters for inode allocation heuristics, taking into account
 * filesystem size and inode32/inode64 mount options; i.e. specifically
 * whether or not XFS_MOUNT_SMALL_INUMS is set.
 *
 * Inode allocation patterns are altered only if inode32 is requested
 * (XFS_MOUNT_SMALL_INUMS), and the filesystem is sufficiently large.
 * If altered, XFS_MOUNT_32BITINODES is set as well.
 *
 * An agcount independent of that in the mount structure is provided
 * because in the growfs case, mp->m_sb.sb_agcount is not yet updated
 * to the potentially higher ag count.
 *
 * Returns the maximum AG index which may contain inodes.
 *
 * NOTE: userspace has no concept of "inode32" and so xfs_has_small_inums
 * is always false, and much of this code is a no-op.
 */
xfs_agnumber_t
xfs_set_inode_alloc(
	struct xfs_mount *mp,
	xfs_agnumber_t	agcount)
{
	xfs_agnumber_t	index;
	xfs_agnumber_t	maxagi = 0;
	xfs_sb_t	*sbp = &mp->m_sb;
	xfs_agnumber_t	max_metadata;
	xfs_agino_t	agino;
	xfs_ino_t	ino;

	/*
	 * Calculate how much should be reserved for inodes to meet
	 * the max inode percentage.  Used only for inode32.
	 */
	if (M_IGEO(mp)->maxicount) {
		uint64_t	icount;

		icount = sbp->sb_dblocks * sbp->sb_imax_pct;
		do_div(icount, 100);
		icount += sbp->sb_agblocks - 1;
		do_div(icount, sbp->sb_agblocks);
		max_metadata = icount;
	} else {
		max_metadata = agcount;
	}

	/* Get the last possible inode in the filesystem */
	agino =	XFS_AGB_TO_AGINO(mp, sbp->sb_agblocks - 1);
	ino = XFS_AGINO_TO_INO(mp, agcount - 1, agino);

	/*
	 * If user asked for no more than 32-bit inodes, and the fs is
	 * sufficiently large, set XFS_MOUNT_32BITINODES if we must alter
	 * the allocator to accommodate the request.
	 */
	if (xfs_has_small_inums(mp) && ino > XFS_MAXINUMBER_32)
		set_bit(XFS_OPSTATE_INODE32, &mp->m_opstate);
	else
		clear_bit(XFS_OPSTATE_INODE32, &mp->m_opstate);

	for (index = 0; index < agcount; index++) {
		struct xfs_perag	*pag;

		ino = XFS_AGINO_TO_INO(mp, index, agino);

		pag = xfs_perag_get(mp, index);
		if (xfs_set_inode_alloc_perag(pag, ino, max_metadata))
			maxagi++;
		xfs_perag_put(pag);
	}

	return xfs_is_inode32(mp) ? maxagi : agcount;
}

static struct xfs_buftarg *
libxfs_buftarg_alloc(
	struct xfs_mount	*mp,
	struct libxfs_dev	*dev,
	unsigned long		write_fails)
{
	struct xfs_buftarg	*btp;

	btp = malloc(sizeof(*btp));
	if (!btp) {
		fprintf(stderr, _("%s: buftarg init failed\n"),
			progname);
		exit(1);
	}
	btp->bt_mount = mp;
	btp->bt_bdev = dev->dev;
	btp->bt_bdev_fd = dev->fd;
	btp->flags = 0;
	if (write_fails) {
		btp->writes_left = write_fails;
		btp->flags |= XFS_BUFTARG_INJECT_WRITE_FAIL;
	}
	pthread_mutex_init(&btp->lock, NULL);

	return btp;
}

enum libxfs_write_failure_nums {
	WF_DATA = 0,
	WF_LOG,
	WF_RT,
	WF_MAX_OPTS,
};

static char *wf_opts[] = {
	[WF_DATA]		= "ddev",
	[WF_LOG]		= "logdev",
	[WF_RT]			= "rtdev",
	[WF_MAX_OPTS]		= NULL,
};

void
libxfs_buftarg_init(
	struct xfs_mount	*mp,
	struct libxfs_init	*xi)
{
	char			*p = getenv("LIBXFS_DEBUG_WRITE_CRASH");
	unsigned long		dfail = 0, lfail = 0, rfail = 0;

	/* Simulate utility crash after a certain number of writes. */
	while (p && *p) {
		char *val;

		switch (getsubopt(&p, wf_opts, &val)) {
		case WF_DATA:
			if (!val) {
				fprintf(stderr,
		_("ddev write fail requires a parameter\n"));
				exit(1);
			}
			dfail = strtoul(val, NULL, 0);
			break;
		case WF_LOG:
			if (!val) {
				fprintf(stderr,
		_("logdev write fail requires a parameter\n"));
				exit(1);
			}
			lfail = strtoul(val, NULL, 0);
			break;
		case WF_RT:
			if (!val) {
				fprintf(stderr,
		_("rtdev write fail requires a parameter\n"));
				exit(1);
			}
			rfail = strtoul(val, NULL, 0);
			break;
		default:
			fprintf(stderr, _("unknown write fail type %s\n"),
					val);
			exit(1);
			break;
		}
	}

	if (mp->m_ddev_targp) {
		/* should already have all buftargs initialised */
		if (mp->m_ddev_targp->bt_bdev != xi->data.dev ||
		    mp->m_ddev_targp->bt_mount != mp) {
			fprintf(stderr,
				_("%s: bad buftarg reinit, ddev\n"),
				progname);
			exit(1);
		}
		if (!xi->log.dev || xi->log.dev == xi->data.dev) {
			if (mp->m_logdev_targp != mp->m_ddev_targp) {
				fprintf(stderr,
				_("%s: bad buftarg reinit, ldev mismatch\n"),
					progname);
				exit(1);
			}
		} else if (mp->m_logdev_targp->bt_bdev != xi->log.dev ||
			   mp->m_logdev_targp->bt_mount != mp) {
			fprintf(stderr,
				_("%s: bad buftarg reinit, logdev\n"),
				progname);
			exit(1);
		}
		if (xi->rt.dev &&
		    (mp->m_rtdev_targp->bt_bdev != xi->rt.dev ||
		     mp->m_rtdev_targp->bt_mount != mp)) {
			fprintf(stderr,
				_("%s: bad buftarg reinit, rtdev\n"),
				progname);
			exit(1);
		}
		return;
	}

	mp->m_ddev_targp = libxfs_buftarg_alloc(mp, &xi->data, dfail);
	if (!xi->log.dev || xi->log.dev == xi->data.dev)
		mp->m_logdev_targp = mp->m_ddev_targp;
	else
		mp->m_logdev_targp = libxfs_buftarg_alloc(mp, &xi->log, lfail);
	mp->m_rtdev_targp = libxfs_buftarg_alloc(mp, &xi->rt, rfail);
}

/* Compute maximum possible height for per-AG btree types for this fs. */
static inline void
xfs_agbtree_compute_maxlevels(
	struct xfs_mount	*mp)
{
	unsigned int		levels;

	levels = max(mp->m_alloc_maxlevels, M_IGEO(mp)->inobt_maxlevels);
	levels = max(levels, mp->m_rmap_maxlevels);
	mp->m_agbtree_maxlevels = max(levels, mp->m_refc_maxlevels);
}

/* Compute maximum possible height of all btrees. */
void
libxfs_compute_all_maxlevels(
	struct xfs_mount	*mp)
{
	xfs_alloc_compute_maxlevels(mp);
	xfs_bmap_compute_maxlevels(mp, XFS_DATA_FORK);
	xfs_bmap_compute_maxlevels(mp, XFS_ATTR_FORK);
	xfs_ialloc_setup_geometry(mp);
	xfs_rmapbt_compute_maxlevels(mp);
	xfs_refcountbt_compute_maxlevels(mp);

	xfs_agbtree_compute_maxlevels(mp);
}

/*
 * precalculate the low space thresholds for dynamic speculative preallocation.
 */
static void
xfs_set_low_space_thresholds(
	struct xfs_mount	*mp)
{
	uint64_t		dblocks = mp->m_sb.sb_dblocks;
	int			i;

	do_div(dblocks, 100);

	for (i = 0; i < XFS_LOWSP_MAX; i++)
		mp->m_low_space[i] = dblocks * (i + 1);
}

/*
 * Mount structure initialization, provides a filled-in xfs_mount_t
 * such that the numerous XFS_* macros can be used.  If dev is zero,
 * no IO will be performed (no size checks, read root inodes).
 */
struct xfs_mount *
libxfs_mount(
	struct xfs_mount	*mp,
	struct xfs_sb		*sb,
	struct libxfs_init	*xi,
	unsigned int		flags)
{
	struct xfs_buf		*bp;
	struct xfs_sb		*sbp;
	xfs_daddr_t		d;
	int			error;

	mp->m_features = xfs_sb_version_to_features(sb);
	if (flags & LIBXFS_MOUNT_DEBUGGER)
		xfs_set_debugger(mp);
	if (flags & LIBXFS_MOUNT_REPORT_CORRUPTION)
		xfs_set_reporting_corruption(mp);
	libxfs_buftarg_init(mp, xi);

	mp->m_finobt_nores = true;
	xfs_set_inode32(mp);
	mp->m_sb = *sb;
	INIT_RADIX_TREE(&mp->m_perag_tree, GFP_KERNEL);
	sbp = &mp->m_sb;
	spin_lock_init(&mp->m_sb_lock);
	spin_lock_init(&mp->m_agirotor_lock);

	xfs_sb_mount_common(mp, sb);

	/*
	 * Set whether we're using stripe alignment.
	 */
	if (xfs_has_dalign(mp)) {
		mp->m_dalign = sbp->sb_unit;
		mp->m_swidth = sbp->sb_width;
	}

	libxfs_compute_all_maxlevels(mp);

	/*
	 * Check that the data (and log if separate) are an ok size.
	 */
	d = (xfs_daddr_t) XFS_FSB_TO_BB(mp, mp->m_sb.sb_dblocks);
	if (XFS_BB_TO_FSB(mp, d) != mp->m_sb.sb_dblocks) {
		fprintf(stderr, _("%s: size check failed\n"), progname);
		if (!xfs_is_debugger(mp))
			return NULL;
	}

	/*
	 * We automatically convert v1 inodes to v2 inodes now, so if
	 * the NLINK bit is not set we can't operate on the filesystem.
	 */
	if (!(sbp->sb_versionnum & XFS_SB_VERSION_NLINKBIT)) {

		fprintf(stderr, _(
	"%s: V1 inodes unsupported. Please try an older xfsprogs.\n"),
				 progname);
		exit(1);
	}

	/* Check for supported directory formats */
	if (!(sbp->sb_versionnum & XFS_SB_VERSION_DIRV2BIT)) {

		fprintf(stderr, _(
	"%s: V1 directories unsupported. Please try an older xfsprogs.\n"),
				 progname);
		exit(1);
	}

	/* check for unsupported other features */
	if (!xfs_sb_good_version(sbp)) {
		fprintf(stderr, _(
	"%s: Unsupported features detected. Please try a newer xfsprogs.\n"),
				 progname);
		exit(1);
	}

	xfs_da_mount(mp);

	/* Initialize the precomputed transaction reservations values */
	xfs_trans_init(mp);

	if (xi->data.dev == 0)	/* maxtrres, we have no device so leave now */
		return mp;

	/* device size checks must pass unless we're a debugger. */
	error = libxfs_buf_read(mp->m_dev, d - XFS_FSS_TO_BB(mp, 1),
			XFS_FSS_TO_BB(mp, 1), 0, &bp, NULL);
	if (error) {
		fprintf(stderr, _("%s: data size check failed\n"), progname);
		if (!xfs_is_debugger(mp))
			return NULL;
	} else
		libxfs_buf_relse(bp);

	if (mp->m_logdev_targp->bt_bdev &&
	    mp->m_logdev_targp->bt_bdev != mp->m_ddev_targp->bt_bdev) {
		d = (xfs_daddr_t) XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks);
		if (XFS_BB_TO_FSB(mp, d) != mp->m_sb.sb_logblocks ||
		    libxfs_buf_read(mp->m_logdev_targp,
				d - XFS_FSB_TO_BB(mp, 1), XFS_FSB_TO_BB(mp, 1),
				0, &bp, NULL)) {
			fprintf(stderr, _("%s: log size checks failed\n"),
					progname);
			if (!xfs_is_debugger(mp))
				return NULL;
		}
		if (bp)
			libxfs_buf_relse(bp);
	}

	xfs_set_low_space_thresholds(mp);

	/* Initialize realtime fields in the mount structure */
	if (rtmount_init(mp)) {
		fprintf(stderr, _("%s: realtime device init failed\n"),
			progname);
			return NULL;
	}

	/*
	 * libxfs_initialize_perag will allocate a perag structure for each ag.
	 * If agcount is corrupted and insanely high, this will OOM the box.
	 * If the agount seems (arbitrarily) high, try to read what would be
	 * the last AG, and if that fails for a relatively high agcount, just
	 * read the first one and let the user know to check the geometry.
	 */
	if (sbp->sb_agcount > 1000000) {
		error = libxfs_buf_read(mp->m_dev,
				XFS_AG_DADDR(mp, sbp->sb_agcount - 1, 0), 1,
				0, &bp, NULL);
		if (error) {
			fprintf(stderr, _("%s: read of AG %u failed\n"),
						progname, sbp->sb_agcount);
			if (!xfs_is_debugger(mp))
				return NULL;
			fprintf(stderr, _("%s: limiting reads to AG 0\n"),
								progname);
			sbp->sb_agcount = 1;
		} else
			libxfs_buf_relse(bp);
	}

	error = libxfs_initialize_perag(mp, sbp->sb_agcount, sbp->sb_dblocks,
			&mp->m_maxagi);
	if (error) {
		fprintf(stderr, _("%s: perag init failed\n"),
			progname);
		exit(1);
	}
	xfs_set_perag_data_loaded(mp);

	return mp;
}

void
libxfs_rtmount_destroy(xfs_mount_t *mp)
{
	if (mp->m_rsumip)
		libxfs_irele(mp->m_rsumip);
	if (mp->m_rbmip)
		libxfs_irele(mp->m_rbmip);
	mp->m_rsumip = mp->m_rbmip = NULL;
}

/* Flush a device and report on writes that didn't make it to stable storage. */
static inline int
libxfs_flush_buftarg(
	struct xfs_buftarg	*btp,
	const char		*buftarg_descr)
{
	int			error = 0;
	int			err2;

	/*
	 * Write verifier failures are evidence of a buggy program.  Make sure
	 * that this state is always reported to the caller.
	 */
	if (btp->flags & XFS_BUFTARG_CORRUPT_WRITE) {
		fprintf(stderr,
_("%s: Refusing to write a corrupt buffer to the %s!\n"),
				progname, buftarg_descr);
		error = -EFSCORRUPTED;
	}

	if (btp->flags & XFS_BUFTARG_LOST_WRITE) {
		fprintf(stderr,
_("%s: Lost a write to the %s!\n"),
				progname, buftarg_descr);
		if (!error)
			error = -EIO;
	}

	err2 = libxfs_blkdev_issue_flush(btp);
	if (err2) {
		fprintf(stderr,
_("%s: Flushing the %s failed, err=%d!\n"),
				progname, buftarg_descr, -err2);
	}
	if (!error)
		error = err2;

	return error;
}

/*
 * Flush all dirty buffers to stable storage and report on writes that didn't
 * make it to stable storage.
 */
int
libxfs_flush_mount(
	struct xfs_mount	*mp)
{
	int			error = 0;
	int			err2;

	/*
	 * Flush the buffer cache to write all dirty buffers to disk.  Buffers
	 * that fail write verification will cause the CORRUPT_WRITE flag to be
	 * set in the buftarg.  Buffers that cannot be written will cause the
	 * LOST_WRITE flag to be set in the buftarg.  Once that's done,
	 * instruct the disks to persist their write caches.
	 */
	libxfs_bcache_flush();

	/* Flush all kernel and disk write caches, and report failures. */
	if (mp->m_ddev_targp) {
		err2 = libxfs_flush_buftarg(mp->m_ddev_targp, _("data device"));
		if (!error)
			error = err2;
	}

	if (mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp) {
		err2 = libxfs_flush_buftarg(mp->m_logdev_targp,
				_("log device"));
		if (!error)
			error = err2;
	}

	if (mp->m_rtdev_targp) {
		err2 = libxfs_flush_buftarg(mp->m_rtdev_targp,
				_("realtime device"));
		if (!error)
			error = err2;
	}

	return error;
}

/*
 * Release any resource obtained during a mount.
 */
int
libxfs_umount(
	struct xfs_mount	*mp)
{
	int			error;

	libxfs_rtmount_destroy(mp);

	/*
	 * Purge the buffer cache to write all dirty buffers to disk and free
	 * all incore buffers, then pick up the outcome when we tell the disks
	 * to persist their write caches.
	 */
	libxfs_bcache_purge();
	error = libxfs_flush_mount(mp);

	/*
	 * Only try to free the per-AG structures if we set them up in the
	 * first place.
	 */
	if (xfs_is_perag_data_loaded(mp))
		libxfs_free_perag(mp);

	kmem_free(mp->m_attr_geo);
	kmem_free(mp->m_dir_geo);

	kmem_free(mp->m_rtdev_targp);
	if (mp->m_logdev_targp != mp->m_ddev_targp)
		kmem_free(mp->m_logdev_targp);
	kmem_free(mp->m_ddev_targp);

	return error;
}

/*
 * Release any global resources used by libxfs.
 */
void
libxfs_destroy(
	struct libxfs_init	*li)
{
	int			leaked;

	libxfs_close_devices(li);

	/* Free everything from the buffer cache before freeing buffer cache */
	libxfs_bcache_purge();
	libxfs_bcache_free();
	cache_destroy(libxfs_bcache);
	leaked = destroy_caches();
	rcu_unregister_thread();
	if (getenv("LIBXFS_LEAK_CHECK") && leaked)
		exit(1);
}

int
libxfs_device_alignment(void)
{
	return platform_align_blockdev();
}

void
libxfs_report(FILE *fp)
{
	time_t t;
	char *c;

	cache_report(fp, "libxfs_bcache", libxfs_bcache);

	t = time(NULL);
	c = asctime(localtime(&t));
	fprintf(fp, "%s", c);
}
