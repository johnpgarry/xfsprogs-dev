// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "libxfs/xfile.h"
#include "libxfs/xfblob.h"
#include "libfrog/platform.h"
#include "repair/err_protos.h"
#include "repair/slab.h"
#include "repair/pptr.h"
#include "repair/strblobs.h"

#undef PPTR_DEBUG

#ifdef PPTR_DEBUG
# define dbg_printf(f, a...)  do {printf(f, ## a); fflush(stdout); } while (0)
#else
# define dbg_printf(f, a...)
#endif

/*
 * Parent Pointer Validation
 * =========================
 *
 * Phase 6 validates the connectivity of the directory tree after validating
 * that all the space metadata are correct, and confirming all the inodes that
 * we intend to keep.  The first part of phase 6 walks the directories of the
 * filesystem to ensure that every file that isn't the root directory has a
 * parent.  Unconnected files are attached to the orphanage.  Filesystems with
 * the directory parent pointer feature enabled must also ensure that for every
 * directory entry that points to a child file, that child has a matching
 * parent pointer.
 *
 * There are many ways that we could check the parent pointers, but the means
 * that we have chosen is to build a per-AG master index of all parent pointers
 * of all inodes stored in that AG, and use that as the basis for comparison.
 * This consumes a lot of memory, but performing both a forward scan to check
 * dirent -> parent pointer and a backwards scan of parent pointer -> dirent
 * takes longer than the simple method presented here.  Userspace adds the
 * additional twist that inodes are not cached (and there are no ILOCKs), which
 * makes that approach even less attractive.
 *
 * During the directory walk at the start of phase 6, we transform each child
 * directory entry found into its parent pointer equivalent.  In other words,
 * the forward information:
 *
 *     (dir_ino, name, child_ino)
 *
 * becomes this backwards information:
 *
 *     (child_agino*, dir_ino*, dir_gen, name_cookie*)
 *
 * Key fields are starred.
 *
 * This tuple is recorded in the per-AG master parent pointer index.  Note
 * that names are stored separately in an xfblob data structure so that the
 * rest of the information can be sorted and processed as fixed-size records;
 * the incore parent pointer record contains a pointer to the strblob data.
 * Because string blobs are deduplicated, there's a 1:1 mapping of name cookies
 * to strings, which means that we can use the name cookie as a comparison key
 * instead of loading the full dentry name every time we want to perform a
 * comparison.
 */

struct ag_pptr {
	/* parent directory handle */
	xfs_ino_t		parent_ino;
	unsigned int		parent_gen;

	/* dirent name length */
	unsigned int		namelen;

	/* cookie for the actual dirent name */
	xfblob_cookie		name_cookie;

	/* agino of the child file */
	xfs_agino_t		child_agino;

	/* hash of the dirent name */
	xfs_dahash_t		namehash;
};

struct ag_pptrs {
	/* Lock to protect pptr_recs during the dirent scan. */
	pthread_mutex_t		lock;

	/* Parent pointer records for files in this AG. */
	struct xfs_slab		*pptr_recs;
};

/* Global names storage file. */
static struct strblobs	*nameblobs;
static pthread_mutex_t	names_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ag_pptrs	*fs_pptrs;

void
parent_ptr_free(
	struct xfs_mount	*mp)
{
	xfs_agnumber_t		agno;

	if (!xfs_has_parent(mp))
		return;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		free_slab(&fs_pptrs[agno].pptr_recs);
		pthread_mutex_destroy(&fs_pptrs[agno].lock);
	}
	free(fs_pptrs);
	fs_pptrs = NULL;

	strblobs_destroy(&nameblobs);
}

void
parent_ptr_init(
	struct xfs_mount	*mp)
{
	char			*descr;
	uint64_t		iused;
	xfs_agnumber_t		agno;
	int			error;

	if (!xfs_has_parent(mp))
		return;

	/* One hash bucket per inode, up to about 8M of memory on 64-bit. */
	iused = min(mp->m_sb.sb_icount - mp->m_sb.sb_ifree, 1048573);
	descr = kasprintf("xfs_repair (%s): parent pointer names",
			mp->m_fsname);
	error = strblobs_init(descr, iused, &nameblobs);
	kfree(descr);
	if (error)
		do_error(_("init parent pointer names failed: %s\n"),
				strerror(error));

	fs_pptrs = calloc(mp->m_sb.sb_agcount, sizeof(struct ag_pptrs));
	if (!fs_pptrs)
		do_error(
 _("init parent pointer per-AG record array failed: %s\n"),
				strerror(errno));

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		error = pthread_mutex_init(&fs_pptrs[agno].lock, NULL);
		if (error)
			do_error(
 _("init agno %u parent pointer lock failed: %s\n"),
					agno, strerror(error));

		error = -init_slab(&fs_pptrs[agno].pptr_recs,
				sizeof(struct ag_pptr));
		if (error)
			do_error(
 _("init agno %u parent pointer recs failed: %s\n"),
					agno, strerror(error));
	}
}

/* Remember that @dp has a dirent (@fname, @ino). */
void
add_parent_ptr(
	xfs_ino_t		ino,
	const unsigned char	*fname,
	struct xfs_inode	*dp)
{
	struct xfs_mount	*mp = dp->i_mount;
	struct xfs_name		dname = {
		.name		= fname,
		.len		= strlen(fname),
	};
	struct ag_pptr		ag_pptr = {
		.child_agino	= XFS_INO_TO_AGINO(mp, ino),
		.parent_ino	= dp->i_ino,
		.parent_gen	= VFS_I(dp)->i_generation,
		.namelen	= dname.len,
	};
	struct ag_pptrs		*ag_pptrs;
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, ino);
	int			error;

	if (!xfs_has_parent(mp))
		return;

	ag_pptr.namehash = libxfs_dir2_hashname(mp, &dname);

	pthread_mutex_lock(&names_mutex);
	error = strblobs_store(nameblobs, &ag_pptr.name_cookie, fname,
			ag_pptr.namelen, ag_pptr.namehash);
	pthread_mutex_unlock(&names_mutex);
	if (error)
		do_error(_("storing name '%s' failed: %s\n"),
				fname, strerror(error));

	ag_pptrs = &fs_pptrs[agno];
	pthread_mutex_lock(&ag_pptrs->lock);
	error = -slab_add(ag_pptrs->pptr_recs, &ag_pptr);
	pthread_mutex_unlock(&ag_pptrs->lock);
	if (error)
		do_error(_("storing name '%s' key failed: %s\n"),
				fname, strerror(error));

	dbg_printf(
 _("%s: dp %llu gen 0x%x fname '%s' ino %llu namecookie 0x%llx\n"),
			__func__,
			(unsigned long long)dp->i_ino,
			VFS_I(dp)->i_generation,
			fname,
			(unsigned long long)ino,
			(unsigned long long)ag_pptr.name_cookie);
}
