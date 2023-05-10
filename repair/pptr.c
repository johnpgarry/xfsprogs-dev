// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "libxfs/xfile.h"
#include "libxfs/xfblob.h"
#include "libfrog/platform.h"
#include "libfrog/workqueue.h"
#include "repair/globals.h"
#include "repair/err_protos.h"
#include "repair/slab.h"
#include "repair/listxattr.h"
#include "repair/threads.h"
#include "repair/incore.h"
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
 *
 * Once we've finished with the forward scan, we get to work on the backwards
 * scan.  Each AG is processed independently.  First, we sort the per-AG master
 * records in order of child_agino, dir_ino, and name_cookie.  Each inode in
 * the AG is then processed in numerical order.
 *
 * The first thing that happens to the file is that we read all the extended
 * attributes to look for parent pointers.  Attributes that claim to be parent
 * pointers but are obviously garbage are thrown away.  The rest of the ondisk
 * parent pointers for that file are stored in memory like this:
 *
 *     (dir_ino*, dir_gen, name_cookie*)
 *
 * After loading the ondisk parent pointer name, we search the strblobs
 * structure to see if it has already recorded the name.  If so, this value is
 * used as the name cookie.  If the name has not yet been recorded, we flag the
 * incore record for later deletion.
 *
 * When we've concluded the xattr scan, the per-file records are sorted in
 * order of dir_ino and name_cookie.
 *
 * There are three possibilities here:
 *
 * A. The first record in the per-AG master index is an exact match for the
 * first record in the per-file index.  Everything is consistent, and we can
 * proceed with the lockstep scan detailed below.
 *
 * B. The per-AG master index cursor points to a higher inode number than the
 * first inode we are scanning.  Delete the ondisk parent pointers
 * corresponding to the per-file records until condition (B) is no longer true.
 *
 * C. The per-AG master index cursor instead points to a lower inode number
 * than the one we are scanning.  This means that there exists a directory
 * entry pointing at an inode that is free.  We supposedly already settled
 * which inodes are free and which aren't, which means in-memory information is
 * inconsistent.  Abort.
 *
 * Otherwise, we are ready to check the file parent pointers against the
 * master.  If the ondisk directory metadata are all consistent, this recordset
 * should correspond exactly to the subset of the master records with a
 * child_agino matching the file that we're scanning.  We should be able to
 * walk both sets in lockstep, and find one of the following outcomes:
 *
 * 1) The master index cursor is ahead of the ondisk index cursor.  This means
 * that the inode has parent pointers that were not found during the dirent
 * scan.  These should be deleted.
 *
 * 2) The ondisk index gets ahead of the master index.  This means that the
 * dirent scan found parent pointers that are not attached to the inode.
 * These should be added.
 *
 * 3) The parent_gen or (dirent) name are not consistent.  Update the parent
 * pointer to the values that we found during the dirent scan.
 *
 * 4) Everything matches.  Move on to the next parent pointer.
 *
 * The current implementation does not try to rebuild directories from parent
 * pointer information, as this requires a lengthy scan of the filesystem for
 * each broken directory.
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

struct file_pptr {
	/* parent directory handle */
	unsigned long long	parent_ino;
	unsigned int		parent_gen;

	/* Is the name stored in the global nameblobs structure? */
	unsigned int		name_in_nameblobs;

	/* hash of the dirent name */
	xfs_dahash_t		namehash;

	/* parent pointer name length */
	unsigned int		namelen;

	/* cookie for the file dirent name */
	xfblob_cookie		name_cookie;
};

struct ag_pptrs {
	/* Lock to protect pptr_recs during the dirent scan. */
	pthread_mutex_t		lock;

	/* Parent pointer records for files in this AG. */
	struct xfs_slab		*pptr_recs;
};

struct file_scan {
	struct ag_pptrs		*ag_pptrs;

	/* cursor for comparing ag_pptrs.pptr_recs against file_pptrs_recs */
	struct xfs_slab_cursor	*ag_pptr_recs_cur;

	/* xfs_parent_name_rec records for a file that we're checking */
	struct xfs_slab		*file_pptr_recs;

	/* cursor for comparing file_pptr_recs against pptrs_recs */
	struct xfs_slab_cursor	*file_pptr_recs_cur;

	/* names associated with file_pptr_recs */
	struct xfblob		*file_pptr_names;

	/* Number of parent pointers recorded for this file. */
	unsigned int		nr_file_pptrs;

	/* Does this file have garbage xattrs with ATTR_PARENT set? */
	bool			have_garbage;

	/* xattrs that we have to remove from this file */
	struct xfs_slab		*garbage_xattr_recs;

	/* attr names associated with garbage_xattr_recs */
	struct xfblob		*garbage_xattr_names;
};

struct garbage_xattr {
	/* xfs_da_args.attr_filter for the attribute being removed */
	unsigned int		attr_filter;

	/* attribute name length */
	unsigned int		attrnamelen;

	/* attribute value length */
	unsigned int		attrvaluelen;

	/* cookie for the attribute name */
	xfblob_cookie		attrname_cookie;

	/* cookie for the attribute value */
	xfblob_cookie		attrvalue_cookie;
};

/* Global names storage file. */
static struct strblobs	*nameblobs;
static pthread_mutex_t	names_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ag_pptrs	*fs_pptrs;

static int
cmp_ag_pptr(
	const void		*a,
	const void		*b)
{
	const struct ag_pptr	*pa = a;
	const struct ag_pptr	*pb = b;

	if (pa->child_agino < pb->child_agino)
		return -1;
	if (pa->child_agino > pb->child_agino)
		return 1;

	if (pa->parent_ino < pb->parent_ino)
		return -1;
	if (pa->parent_ino > pb->parent_ino)
		return 1;

	if (pa->namehash < pb->namehash)
		return -1;
	if (pa->namehash > pb->namehash)
		return 1;

	if (pa->name_cookie < pb->name_cookie)
		return -1;
	if (pa->name_cookie > pb->name_cookie)
		return 1;

	return 0;
}

static int
cmp_file_pptr(
	const void		*a,
	const void		*b)
{
	const struct file_pptr	*pa = a;
	const struct file_pptr	*pb = b;

	if (pa->parent_ino < pb->parent_ino)
		return -1;
	if (pa->parent_ino > pb->parent_ino)
		return 1;

	/*
	 * Push the parent pointer names that we didn't find in the dirent scan
	 * towards the end of the list so that we delete them as excess.
	 */
	if (!pa->name_in_nameblobs && pb->name_in_nameblobs)
		return 1;
	if (pa->name_in_nameblobs && !pb->name_in_nameblobs)
		return -1;

	if (pa->namehash < pb->namehash)
		return -1;
	if (pa->namehash > pb->namehash)
		return 1;

	if (pa->name_cookie < pb->name_cookie)
		return -1;
	if (pa->name_cookie > pb->name_cookie)
		return 1;

	return 0;
}

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

/* Remove garbage extended attributes that have ATTR_PARENT set. */
static void
remove_garbage_xattrs(
	struct xfs_inode	*ip,
	struct file_scan	*fscan)
{
	struct xfs_slab_cursor	*cur;
	struct garbage_xattr	*ga;
	void			*buf = NULL;
	size_t			bufsize = 0;
	int			error;

	error = -init_slab_cursor(fscan->garbage_xattr_recs, NULL, &cur);
	if (error)
		do_error(_("init garbage xattr cursor failed: %s\n"),
				strerror(error));

	while ((ga = pop_slab_cursor(cur)) != NULL) {
		struct xfs_da_args	args = {
			.dp		= ip,
			.attr_filter	= ga->attr_filter,
			.namelen	= ga->attrnamelen,
			.valuelen	= ga->attrvaluelen,
			.op_flags	= XFS_DA_OP_REMOVE | XFS_DA_OP_NVLOOKUP,
		};
		size_t		desired = ga->attrnamelen + ga->attrvaluelen;

		if (desired > bufsize) {
			free(buf);
			buf = malloc(desired);
			if (!buf)
				do_error(
 _("allocating %zu bytes to remove ino %llu garbage xattr failed: %s\n"),
						desired,
						(unsigned long long)ip->i_ino,
						strerror(errno));
			bufsize = desired;
		}

		args.name = buf;
		args.value = buf + ga->attrnamelen;

		error = -xfblob_load(fscan->garbage_xattr_names,
				ga->attrname_cookie, buf, ga->attrnamelen);
		if (error)
			do_error(
 _("loading garbage xattr name failed: %s\n"),
					strerror(error));

		error = -xfblob_load(fscan->garbage_xattr_names,
				ga->attrvalue_cookie, args.value,
				ga->attrvaluelen);
		if (error)
			do_error(
 _("loading garbage xattr value failed: %s\n"),
					strerror(error));

		error = -libxfs_attr_set(&args);
		if (error)
			do_error(
 _("removing ino %llu garbage xattr failed: %s\n"),
					(unsigned long long)ip->i_ino,
					strerror(error));
	}

	free(buf);
	free_slab_cursor(&cur);
	free_slab(&fscan->garbage_xattr_recs);
	xfblob_destroy(fscan->garbage_xattr_names);
	fscan->garbage_xattr_names = NULL;
}

/* Schedule this ATTR_PARENT extended attribute for deletion. */
static void
record_garbage_xattr(
	struct xfs_inode	*ip,
	struct file_scan	*fscan,
	unsigned int		attr_filter,
	const char		*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen)
{
	struct garbage_xattr	garbage_xattr = {
		.attr_filter	= attr_filter,
		.attrnamelen	= namelen,
		.attrvaluelen	= valuelen,
	};
	struct xfs_mount	*mp = ip->i_mount;
	char			*descr;
	int			error;

	if (no_modify) {
		if (!fscan->have_garbage)
			do_warn(
 _("would delete garbage parent pointer extended attributes in ino %llu\n"),
					(unsigned long long)ip->i_ino);
		fscan->have_garbage = true;
		return;
	}

	if (fscan->have_garbage)
		goto stuffit;
	fscan->have_garbage = true;

	do_warn(
 _("deleting garbage parent pointer extended attributes in ino %llu\n"),
			(unsigned long long)ip->i_ino);

	error = -init_slab(&fscan->garbage_xattr_recs,
			sizeof(struct garbage_xattr));
	if (error)
		do_error(_("init garbage xattr recs failed: %s\n"),
				strerror(error));

	descr = kasprintf("xfs_repair (%s): garbage xattr names",
			mp->m_fsname);
	error = -xfblob_create(descr, &fscan->garbage_xattr_names);
	kfree(descr);
	if (error)
		do_error("init garbage xattr names failed: %s\n",
				strerror(error));

stuffit:
	error = -xfblob_store(fscan->garbage_xattr_names,
			&garbage_xattr.attrname_cookie, name, namelen);
	if (error)
		do_error(_("storing ino %llu garbage xattr failed: %s\n"),
				(unsigned long long)ip->i_ino,
				strerror(error));

	error = -xfblob_store(fscan->garbage_xattr_names,
			&garbage_xattr.attrvalue_cookie, value, valuelen);
	if (error)
		do_error(_("storing ino %llu garbage xattr failed: %s\n"),
				(unsigned long long)ip->i_ino,
				strerror(error));

	error = -slab_add(fscan->garbage_xattr_recs, &garbage_xattr);
	if (error)
		do_error(_("storing ino %llu garbage xattr rec failed: %s\n"),
				(unsigned long long)ip->i_ino,
				strerror(error));
}

/*
 * Store this file parent pointer's name in the file scan namelist unless it's
 * already in the global list.
 */
static int
store_file_pptr_name(
	struct file_scan			*fscan,
	struct file_pptr			*file_pptr,
	const struct xfs_parent_name_irec	*irec)
{
	int					error;

	error = strblobs_lookup(nameblobs, &file_pptr->name_cookie,
			irec->p_name, irec->p_namelen, file_pptr->namehash);
	if (!error) {
		file_pptr->name_in_nameblobs = true;
		return 0;
	}
	if (error != ENOENT)
		return error;

	file_pptr->name_in_nameblobs = false;
	return -xfblob_store(fscan->file_pptr_names, &file_pptr->name_cookie,
			irec->p_name, irec->p_namelen);
}

/* Decide if this is a directory parent pointer and stash it if so. */
static int
examine_xattr(
	struct xfs_inode	*ip,
	unsigned int		attr_flags,
	const unsigned char	*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen,
	void			*priv)
{
	struct file_pptr	file_pptr = { };
	struct xfs_name		dname = {
		.name		= value,
		.len		= valuelen,
	};
	struct xfs_parent_name_irec irec;
	struct xfs_mount	*mp = ip->i_mount;
	struct file_scan	*fscan = priv;
	const struct xfs_parent_name_rec *rec = (const void *)name;
	int			error;

	/* Ignore anything that isn't a parent pointer. */
	if (!(attr_flags & XFS_ATTR_PARENT))
		return 0;

	/* No incomplete parent pointers. */
	if (attr_flags & XFS_ATTR_INCOMPLETE)
		goto corrupt;

	/* Does the ondisk parent pointer structure make sense? */
	if (!xfs_parent_namecheck(mp, rec, namelen, attr_flags) ||
	    !xfs_parent_valuecheck(mp, value, valuelen))
		goto corrupt;

	/*
	 * If the namehash of the dirent name encoded in the parent pointer
	 * attr value doesn't match the namehash in the parent pointer key,
	 * delete this attribute.
	 */
	if (!xfs_parent_hashcheck(mp, rec, value, valuelen)) {
		xfs_dahash_t	computed_hash;

		computed_hash = libxfs_dir2_hashname(ip->i_mount, &dname);
		do_warn(
 _("bad hash 0x%x for ino %llu parent pointer '%.*s', expected 0x%x\n"),
				irec.p_namehash,
				(unsigned long long)ip->i_ino,
				irec.p_namelen,
				(const char *)irec.p_name,
				computed_hash);
		goto corrupt;
	}

	libxfs_parent_irec_from_disk(&irec, rec, value, valuelen);

	file_pptr.parent_ino = irec.p_ino;
	file_pptr.parent_gen = irec.p_gen;
	file_pptr.namelen = irec.p_namelen;
	file_pptr.namehash = irec.p_namehash;

	error = store_file_pptr_name(fscan, &file_pptr, &irec);
	if (error)
		do_error(
 _("storing ino %llu parent pointer '%.*s' failed: %s\n"),
				(unsigned long long)ip->i_ino,
				irec.p_namelen,
				(const char *)irec.p_name,
				strerror(error));

	error = -slab_add(fscan->file_pptr_recs, &file_pptr);
	if (error)
		do_error(_("storing ino %llu parent pointer rec failed: %s\n"),
				(unsigned long long)ip->i_ino,
				strerror(error));

	dbg_printf(
 _("%s: dp %llu gen 0x%x fname '%.*s' namelen %u ino %llu namecookie 0x%llx global? %d\n"),
			__func__,
			(unsigned long long)irec.p_ino,
			irec.p_gen,
			irec.p_namelen,
			(const char *)irec.p_name,
			irec.p_namelen,
			(unsigned long long)ip->i_ino,
			(unsigned long long)file_pptr.name_cookie,
			file_pptr.name_in_nameblobs);

	fscan->nr_file_pptrs++;
	return 0;
corrupt:
	record_garbage_xattr(ip, fscan, attr_flags, name, namelen, value,
			valuelen);
	return 0;
}

/* Load a file parent pointer name from wherever we stored it. */
static int
load_file_pptr_name(
	struct file_scan	*fscan,
	const struct file_pptr	*file_pptr,
	unsigned char		*name)
{
	if (file_pptr->name_in_nameblobs)
		return strblobs_load(nameblobs, file_pptr->name_cookie,
				name, file_pptr->namelen);

	return -xfblob_load(fscan->file_pptr_names, file_pptr->name_cookie,
			name, file_pptr->namelen);
}

/* Add an on disk parent pointer to a file. */
static int
add_file_pptr(
	struct xfs_inode		*ip,
	const struct ag_pptr		*ag_pptr,
	const unsigned char		*name)
{
	struct xfs_parent_name_irec	pptr_rec = {
		.p_ino			= ag_pptr->parent_ino,
		.p_gen			= ag_pptr->parent_gen,
		.p_namelen		= ag_pptr->namelen,
	};
	struct xfs_parent_scratch	scratch;
	int				error;

	memcpy(pptr_rec.p_name, name, ag_pptr->namelen);
	libxfs_parent_irec_hashname(ip->i_mount, &pptr_rec);
	error = -libxfs_parent_lookup(NULL, ip, &pptr_rec, &scratch);
	if (!error || error != ENOATTR)
		return error;
	return -libxfs_parent_set(ip, ip->i_ino, &pptr_rec, &scratch);
}

/* Remove an on disk parent pointer from a file. */
static int
remove_file_pptr(
	struct xfs_inode		*ip,
	const struct file_pptr		*file_pptr,
	const unsigned char		*name)
{
	struct xfs_parent_name_irec	pptr_rec = {
		.p_ino			= file_pptr->parent_ino,
		.p_gen			= file_pptr->parent_gen,
		.p_namelen		= file_pptr->namelen,
	};
	struct xfs_parent_scratch	scratch;

	memcpy(pptr_rec.p_name, name, file_pptr->namelen);
	libxfs_parent_irec_hashname(ip->i_mount, &pptr_rec);
	return -libxfs_parent_unset(ip, ip->i_ino, &pptr_rec, &scratch);
}

/* Remove all pptrs from @ip. */
static void
clear_all_pptrs(
	struct xfs_inode	*ip)
{
	if (no_modify) {
		do_warn(_("would delete unlinked ino %llu parent pointers\n"),
				(unsigned long long)ip->i_ino);
		return;
	}

	do_warn(_("deleting unlinked ino %llu parent pointers\n"),
			(unsigned long long)ip->i_ino);
	/* XXX actually do the work */
}

/* Add @ag_pptr to @ip. */
static void
add_missing_parent_ptr(
	struct xfs_inode	*ip,
	struct file_scan	*fscan,
	const struct ag_pptr	*ag_pptr)
{
	unsigned char		name[MAXNAMELEN];
	int			error;

	error = strblobs_load(nameblobs, ag_pptr->name_cookie, name,
			ag_pptr->namelen);
	if (error)
		do_error(
 _("loading missing name for ino %llu parent pointer (ino %llu gen 0x%x namecookie 0x%llx) failed: %s\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)ag_pptr->parent_ino,
				ag_pptr->parent_gen,
				(unsigned long long)ag_pptr->name_cookie,
				strerror(error));

	if (no_modify) {
		do_warn(
 _("would add missing ino %llu parent pointer (ino %llu gen 0x%x name '%.*s')\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)ag_pptr->parent_ino,
				ag_pptr->parent_gen,
				ag_pptr->namelen,
				name);
		return;
	} else {
		do_warn(
 _("adding missing ino %llu parent pointer (ino %llu gen 0x%x name '%.*s')\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)ag_pptr->parent_ino,
				ag_pptr->parent_gen,
				ag_pptr->namelen,
				name);
	}

	error = add_file_pptr(ip, ag_pptr, name);
	if (error)
		do_error(
 _("adding ino %llu pptr (ino %llu gen 0x%x name '%.*s') failed: %s\n"),
			(unsigned long long)ip->i_ino,
			(unsigned long long)ag_pptr->parent_ino,
			ag_pptr->parent_gen,
			ag_pptr->namelen,
			name,
			strerror(error));
}

/* Remove @file_pptr from @ip. */
static void
remove_incorrect_parent_ptr(
	struct xfs_inode	*ip,
	struct file_scan	*fscan,
	const struct file_pptr	*file_pptr)
{
	unsigned char		name[MAXNAMELEN] = { };
	int			error;

	error = load_file_pptr_name(fscan, file_pptr, name);
	if (error)
		do_error(
 _("loading incorrect name for ino %llu parent pointer (ino %llu gen 0x%x namecookie 0x%llx) failed: %s\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)file_pptr->parent_ino,
				file_pptr->parent_gen,
				(unsigned long long)file_pptr->name_cookie,
				strerror(error));

	if (no_modify) {
		do_warn(
 _("would remove bad ino %llu parent pointer (ino %llu gen 0x%x name '%.*s')\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)file_pptr->parent_ino,
				file_pptr->parent_gen,
				file_pptr->namelen,
				name);
		return;
	}

	do_warn(
 _("removing bad ino %llu parent pointer (ino %llu gen 0x%x name '%.*s')\n"),
			(unsigned long long)ip->i_ino,
			(unsigned long long)file_pptr->parent_ino,
			file_pptr->parent_gen,
			file_pptr->namelen,
			name);

	error = remove_file_pptr(ip, file_pptr, name);
	if (error)
		do_error(
 _("removing ino %llu pptr (ino %llu gen 0x%x name '%.*s') failed: %s\n"),
			(unsigned long long)ip->i_ino,
			(unsigned long long)file_pptr->parent_ino,
			file_pptr->parent_gen,
			file_pptr->namelen,
			name,
			strerror(error));
}

/*
 * We found parent pointers that point to the same inode and directory offset.
 * Make sure they have the same generation number and dirent name.
 */
static void
compare_parent_ptrs(
	struct xfs_inode	*ip,
	struct file_scan	*fscan,
	const struct ag_pptr	*ag_pptr,
	const struct file_pptr	*file_pptr)
{
	unsigned char		name1[MAXNAMELEN] = { };
	unsigned char		name2[MAXNAMELEN] = { };
	int			error;

	error = strblobs_load(nameblobs, ag_pptr->name_cookie, name1,
			ag_pptr->namelen);
	if (error)
		do_error(
 _("loading master-list name for ino %llu parent pointer (ino %llu gen 0x%x namecookie 0x%llx namelen %u) failed: %s\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)ag_pptr->parent_ino,
				ag_pptr->parent_gen,
				(unsigned long long)ag_pptr->name_cookie,
				ag_pptr->namelen,
				strerror(error));

	error = load_file_pptr_name(fscan, file_pptr, name2);
	if (error)
		do_error(
 _("loading file-list name for ino %llu parent pointer (ino %llu gen 0x%x namecookie 0x%llx namelen %u) failed: %s\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)file_pptr->parent_ino,
				file_pptr->parent_gen,
				(unsigned long long)file_pptr->name_cookie,
				ag_pptr->namelen,
				strerror(error));

	if (ag_pptr->parent_gen != file_pptr->parent_gen)
		goto reset;
	if (ag_pptr->namelen != file_pptr->namelen)
		goto reset;
	if (ag_pptr->namehash != file_pptr->namehash)
		goto reset;
	if (memcmp(name1, name2, ag_pptr->namelen))
		goto reset;

	return;

reset:
	if (no_modify) {
		do_warn(
 _("would update ino %llu parent pointer (ino %llu gen 0x%x name '%.*s')\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)ag_pptr->parent_ino,
				ag_pptr->parent_gen,
				ag_pptr->namelen,
				name1);
		return;
	}

	do_warn(
 _("updating ino %llu parent pointer (ino %llu gen 0x%x name '%.*s')\n"),
			(unsigned long long)ip->i_ino,
			(unsigned long long)ag_pptr->parent_ino,
			ag_pptr->parent_gen,
			ag_pptr->namelen,
			name1);

	if (ag_pptr->parent_gen != file_pptr->parent_gen ||
	    ag_pptr->namehash   != file_pptr->namehash) {
		error = remove_file_pptr(ip, file_pptr, name2);
		if (error)
			do_error(
 _("erasing ino %llu pptr (ino %llu gen 0x%x name '%.*s') failed: %s\n"),
				(unsigned long long)ip->i_ino,
				(unsigned long long)file_pptr->parent_ino,
				file_pptr->parent_gen,
				file_pptr->namelen,
				name2,
				strerror(error));
	}

	error = add_file_pptr(ip, ag_pptr, name1);
	if (error)
		do_error(
 _("updating ino %llu pptr (ino %llu gen 0x%x name '%.*s') failed: %s\n"),
			(unsigned long long)ip->i_ino,
			(unsigned long long)ag_pptr->parent_ino,
			ag_pptr->parent_gen,
			ag_pptr->namelen,
			name1,
			strerror(error));
}

static int
cmp_file_to_ag_pptr(
	const struct file_pptr	*fp,
	const struct ag_pptr	*ap)
{
	/*
	 * We finished iterating all the pptrs attached to the file before we
	 * ran out of pptrs that we found in the directory scan.  Return 1 so
	 * the caller adds the pptr from the dir scan.
	 */
	if (!fp)
		return 1;

	if (fp->parent_ino > ap->parent_ino)
		return 1;
	if (fp->parent_ino < ap->parent_ino)
		return -1;

	if (fp->namehash < ap->namehash)
		return -1;
	if (fp->namehash > ap->namehash)
		return 1;

	/*
	 * If this parent pointer wasn't found in the dirent scan, we know it
	 * should be removed.
	 */
	if (!fp->name_in_nameblobs)
		return -1;

	if (fp->name_cookie < ap->name_cookie)
		return -1;
	if (fp->name_cookie > ap->name_cookie)
		return 1;

	return 0;
}

/*
 * Make sure that the parent pointers we observed match the ones ondisk.
 *
 * Earlier, we generated a master list of parent pointers for files in this AG
 * based on what we saw during the directory walk at the start of phase 6.
 * Now that we've read in all of this file's parent pointers, make sure the
 * lists match.
 */
static void
crosscheck_file_parent_ptrs(
	struct xfs_inode	*ip,
	struct file_scan	*fscan)
{
	struct ag_pptr		*ag_pptr;
	struct file_pptr	*file_pptr;
	struct xfs_mount	*mp = ip->i_mount;
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, ip->i_ino);
	xfs_agino_t		agino = XFS_INO_TO_AGINO(mp, ip->i_ino);
	int			error;

	ag_pptr = peek_slab_cursor(fscan->ag_pptr_recs_cur);

	if (!ag_pptr || ag_pptr->child_agino > agino) {
		/*
		 * The cursor for the master pptr list has gone beyond this
		 * file that we're scanning.  Evidently it has no parents at
		 * all, so we better not have found any pptrs attached to the
		 * file.
		 */
		if (fscan->nr_file_pptrs > 0)
			clear_all_pptrs(ip);

		return;
	}

	if (ag_pptr->child_agino < agino) {
		/*
		 * The cursor for the master pptr list is behind the file that
		 * we're scanning.  This suggests that the incore inode tree
		 * doesn't know about a file that is mentioned by a dirent.
		 * At this point the inode liveness is supposed to be settled,
		 * which means our incore information is inconsistent.
		 */
		do_error(
 _("found dirent referring to ino %llu even though inobt scan moved on to ino %llu?!\n"),
				(unsigned long long)XFS_AGINO_TO_INO(mp, agno,
					ag_pptr->child_agino),
				(unsigned long long)ip->i_ino);
		/* does not return */
	}

	/*
	 * The master pptr list cursor is pointing to the inode that we want
	 * to check.  Sort the pptr records that we recorded from the ondisk
	 * pptrs for this file, then set up for the comparison.
	 */
	qsort_slab(fscan->file_pptr_recs, cmp_file_pptr);

	error = -init_slab_cursor(fscan->file_pptr_recs, cmp_file_pptr,
			&fscan->file_pptr_recs_cur);
	if (error)
		do_error(_("init ino %llu parent pointer cursor failed: %s\n"),
				(unsigned long long)ip->i_ino, strerror(error));

	do {
		int	cmp_result;

		file_pptr = peek_slab_cursor(fscan->file_pptr_recs_cur);

		dbg_printf(
 _("%s: dp %llu dp_gen 0x%x namelen %u ino %llu namecookie 0x%llx (master)\n"),
				__func__,
				(unsigned long long)ag_pptr->parent_ino,
				ag_pptr->parent_gen,
				ag_pptr->namelen,
				(unsigned long long)ip->i_ino,
				(unsigned long long)ag_pptr->name_cookie);

		if (file_pptr) {
			dbg_printf(
 _("%s: dp %llu dp_gen 0x%x namelen %u ino %llu namecookie 0x%llx (file)\n"),
					__func__,
					(unsigned long long)file_pptr->parent_ino,
					file_pptr->parent_gen,
					file_pptr->namelen,
					(unsigned long long)ip->i_ino,
					(unsigned long long)file_pptr->name_cookie);
		} else {
			dbg_printf(
 _("%s: ran out of parent pointers for ino %llu (file)\n"),
					__func__,
					(unsigned long long)ip->i_ino);
		}

		cmp_result = cmp_file_to_ag_pptr(file_pptr, ag_pptr);
		if (cmp_result > 0) {
			/*
			 * The master pptr list knows about pptrs that are not
			 * in the ondisk metadata.  Add the missing pptr and
			 * advance only the master pptr cursor.
			 */
			add_missing_parent_ptr(ip, fscan, ag_pptr);
			advance_slab_cursor(fscan->ag_pptr_recs_cur);
		} else if (cmp_result < 0) {
			/*
			 * The ondisk pptrs mention a link that is not in the
			 * master list.  Delete the extra pptr and advance only
			 * the file pptr cursor.
			 */
			remove_incorrect_parent_ptr(ip, fscan, file_pptr);
			advance_slab_cursor(fscan->file_pptr_recs_cur);
		} else {
			/*
			 * Exact match, make sure the parent_gen and dirent
			 * name parts of the parent pointer match.  Move both
			 * cursors forward.
			 */
			compare_parent_ptrs(ip, fscan, ag_pptr, file_pptr);
			advance_slab_cursor(fscan->ag_pptr_recs_cur);
			advance_slab_cursor(fscan->file_pptr_recs_cur);
		}

		ag_pptr = peek_slab_cursor(fscan->ag_pptr_recs_cur);
	} while (ag_pptr && ag_pptr->child_agino == agino);

	while ((file_pptr = pop_slab_cursor(fscan->file_pptr_recs_cur))) {
		dbg_printf(
 _("%s: dp %llu dp_gen 0x%x namelen %u ino %llu namecookie 0x%llx (excess)\n"),
				__func__,
				(unsigned long long)file_pptr->parent_ino,
				file_pptr->parent_gen,
				file_pptr->namelen,
				(unsigned long long)ip->i_ino,
				(unsigned long long)file_pptr->name_cookie);

		/*
		 * The master pptr list does not have any more pptrs for this
		 * file, but we still have unprocessed ondisk pptrs.  Delete
		 * all these ondisk pptrs.
		 */
		remove_incorrect_parent_ptr(ip, fscan, file_pptr);
	}
}

/* Ensure this file's parent pointers match what we found in the dirent scan. */
static void
check_file_parent_ptrs(
	struct xfs_inode	*ip,
	struct file_scan	*fscan)
{
	int			error;

	error = -init_slab(&fscan->file_pptr_recs, sizeof(struct file_pptr));
	if (error)
		do_error(_("init file parent pointer recs failed: %s\n"),
				strerror(error));

	fscan->have_garbage = false;
	fscan->nr_file_pptrs = 0;

	error = xattr_walk(ip, examine_xattr, fscan);
	if (error && !no_modify)
		do_error(_("ino %llu parent pointer scan failed: %s\n"),
				(unsigned long long)ip->i_ino,
				strerror(error));
	if (error) {
		do_warn(_("ino %llu parent pointer scan failed: %s\n"),
				(unsigned long long)ip->i_ino,
				strerror(error));
		goto out_free;
	}

	if (!no_modify && fscan->have_garbage)
		remove_garbage_xattrs(ip, fscan);

	crosscheck_file_parent_ptrs(ip, fscan);

out_free:
	free_slab(&fscan->file_pptr_recs);
	xfblob_truncate(fscan->file_pptr_names);
}

/* Check all the parent pointers of files in this AG. */
static void
check_ag_parent_ptrs(
	struct workqueue	*wq,
	uint32_t		agno,
	void			*arg)
{
	struct xfs_mount	*mp = wq->wq_ctx;
	struct file_scan	fscan = {
		.ag_pptrs	= &fs_pptrs[agno],
	};
	struct ag_pptrs		*ag_pptrs = &fs_pptrs[agno];
	struct ino_tree_node	*irec;
	char			*descr;
	int			error;

	qsort_slab(ag_pptrs->pptr_recs, cmp_ag_pptr);

	error = -init_slab_cursor(ag_pptrs->pptr_recs, cmp_ag_pptr,
			&fscan.ag_pptr_recs_cur);
	if (error)
		do_error(
 _("init agno %u parent pointer slab cursor failed: %s\n"),
				agno, strerror(error));

	descr = kasprintf("xfs_repair (%s): file parent pointer names",
			mp->m_fsname);
	error = -xfblob_create(descr, &fscan.file_pptr_names);
	kfree(descr);
	if (error)
		do_error(
 _("init agno %u file parent pointer names failed: %s\n"),
				agno, strerror(error));

	for (irec = findfirst_inode_rec(agno);
	     irec != NULL;
	     irec = next_ino_rec(irec)) {
		unsigned int	ino_offset;

		for (ino_offset = 0;
		     ino_offset < XFS_INODES_PER_CHUNK;
		     ino_offset++) {
			struct xfs_inode *ip;
			xfs_ino_t	ino;

			if (is_inode_free(irec, ino_offset))
				continue;

			ino = XFS_AGINO_TO_INO(mp, agno,
					irec->ino_startnum + ino_offset);
			error = -libxfs_iget(mp, NULL, ino, 0, &ip);
			if (error && !no_modify)
				do_error(
 _("loading ino %llu for parent pointer check failed: %s\n"),
						(unsigned long long)ino,
						strerror(error));
			if (error) {
				do_warn(
 _("loading ino %llu for parent pointer check failed: %s\n"),
						(unsigned long long)ino,
						strerror(error));
				continue;
			}

			check_file_parent_ptrs(ip, &fscan);
			libxfs_irele(ip);
		}
	}

	xfblob_destroy(fscan.file_pptr_names);
	free_slab_cursor(&fscan.ag_pptr_recs_cur);
}

/* Check all the parent pointers of all files in this filesystem. */
void
check_parent_ptrs(
	struct xfs_mount	*mp)
{
	struct workqueue	wq;
	xfs_agnumber_t		agno;

	if (!xfs_has_parent(mp))
		return;

	create_work_queue(&wq, mp, ag_stride);

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++)
		queue_work(&wq, check_ag_parent_ptrs, agno, NULL);

	destroy_work_queue(&wq);
}
