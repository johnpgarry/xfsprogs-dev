// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "type.h"
#include "faddr.h"
#include "fprint.h"
#include "field.h"
#include "inode.h"
#include "io.h"
#include "print.h"
#include "block.h"
#include "bit.h"
#include "output.h"
#include "init.h"
#include "libfrog/bitmap.h"

static int	inode_a_bmbt_count(void *obj, int startoff);
static int	inode_a_bmx_count(void *obj, int startoff);
static int	inode_a_count(void *obj, int startoff);
static int	inode_a_offset(void *obj, int startoff, int idx);
static int	inode_a_sfattr_count(void *obj, int startoff);
static int	inode_core_nlinkv2_count(void *obj, int startoff);
static int	inode_core_onlink_count(void *obj, int startoff);
static int	inode_core_projid_count(void *obj, int startoff);
static int	inode_core_nlinkv1_count(void *obj, int startoff);
static int	inode_core_v3_pad_count(void *obj, int startoff);
static int	inode_core_v2_pad_count(void *obj, int startoff);
static int	inode_core_flushiter_count(void *obj, int startoff);
static int	inode_core_nrext64_pad_count(void *obj, int startoff);
static int	inode_core_nextents_offset(void *obj, int startoff, int idx);
static int	inode_core_nextents32_count(void *obj, int startoff);
static int	inode_core_nextents64_count(void *obj, int startoff);
static int	inode_core_anextents_offset(void *obj, int startoff, int idx);
static int	inode_core_anextents16_count(void *obj, int startoff);
static int	inode_core_anextents32_count(void *obj, int startoff);
static int	inode_f(int argc, char **argv);
static int	inode_u_offset(void *obj, int startoff, int idx);
static int	inode_u_bmbt_count(void *obj, int startoff);
static int	inode_u_bmx_count(void *obj, int startoff);
static int	inode_u_c_count(void *obj, int startoff);
static int	inode_u_dev_count(void *obj, int startoff);
static int	inode_u_muuid_count(void *obj, int startoff);
static int	inode_u_sfdir2_count(void *obj, int startoff);
static int	inode_u_sfdir3_count(void *obj, int startoff);
static int	inode_u_symlink_count(void *obj, int startoff);
static int	inode_u_rtrmapbt_count(void *obj, int startoff);
static int	inode_u_rtrefcbt_count(void *obj, int startoff);

static const cmdinfo_t	inode_cmd =
	{ "inode", NULL, inode_f, 0, 1, 1, "[inode#]",
	  "set current inode", NULL };

const field_t	inode_hfld[] = {
	{ "", FLDT_INODE, OI(0), C1, 0, TYP_NONE },
	{ NULL }
};
const field_t	inode_crc_hfld[] = {
	{ "", FLDT_INODE_CRC, OI(0), C1, 0, TYP_NONE },
	{ NULL }
};

/* XXX: fix this up! */
#define	OFF(f)	bitize(offsetof(struct xfs_dinode, di_ ## f))
const field_t	inode_flds[] = {
	{ "core", FLDT_DINODE_CORE, OI(OFF(magic)), C1, 0, TYP_NONE },
	{ "next_unlinked", FLDT_AGINO, OI(OFF(next_unlinked)), C1, 0,
	  TYP_INODE },
	{ "u", FLDT_DINODE_U, inode_u_offset, C1, FLD_OFFSET, TYP_NONE },
	{ "a", FLDT_DINODE_A, inode_a_offset, inode_a_count,
	  FLD_COUNT|FLD_OFFSET, TYP_NONE },
	{ NULL }
};
const field_t	inode_crc_flds[] = {
	{ "core", FLDT_DINODE_CORE, OI(OFF(magic)), C1, 0, TYP_NONE },
	{ "next_unlinked", FLDT_AGINO, OI(OFF(next_unlinked)), C1, 0,
	  TYP_INODE },
	{ "v3", FLDT_DINODE_V3, OI(OFF(magic)), C1, 0, TYP_NONE },
	{ "u3", FLDT_DINODE_U, inode_u_offset, C1, FLD_OFFSET, TYP_NONE },
	{ "a", FLDT_DINODE_A, inode_a_offset, inode_a_count,
	  FLD_COUNT|FLD_OFFSET, TYP_NONE },
	{ NULL }
};


#define	COFF(f)	bitize(offsetof(struct xfs_dinode, di_ ## f))
const field_t	inode_core_flds[] = {
	{ "magic", FLDT_UINT16X, OI(COFF(magic)), C1, 0, TYP_NONE },
	{ "mode", FLDT_UINT16O, OI(COFF(mode)), C1, 0, TYP_NONE },
	{ "version", FLDT_INT8D, OI(COFF(version)), C1, 0, TYP_NONE },
	{ "format", FLDT_DINODE_FMT, OI(COFF(format)), C1, 0, TYP_NONE },
	{ "nlinkv1", FLDT_UINT16D, OI(COFF(onlink)), inode_core_nlinkv1_count,
	  FLD_COUNT, TYP_NONE },
	{ "onlink", FLDT_UINT16D, OI(COFF(onlink)), inode_core_onlink_count,
	  FLD_COUNT, TYP_NONE },
	{ "uid", FLDT_UINT32D, OI(COFF(uid)), C1, 0, TYP_NONE },
	{ "gid", FLDT_UINT32D, OI(COFF(gid)), C1, 0, TYP_NONE },
	{ "nlinkv2", FLDT_UINT32D, OI(COFF(nlink)), inode_core_nlinkv2_count,
	  FLD_COUNT, TYP_NONE },
	{ "projid_lo", FLDT_UINT16D, OI(COFF(projid_lo)),
	  inode_core_projid_count, FLD_COUNT, TYP_NONE },
	{ "projid_hi", FLDT_UINT16D, OI(COFF(projid_hi)),
	  inode_core_projid_count, FLD_COUNT, TYP_NONE },
	/* union 1 */
	{ "nextents", FLDT_UINT64D, inode_core_nextents_offset,
	  inode_core_nextents64_count, FLD_OFFSET|FLD_COUNT, TYP_NONE },
	{ "v3_pad", FLDT_UINT64D, OI(OFF(v3_pad)),
	  inode_core_v3_pad_count, FLD_COUNT|FLD_SKIPALL, TYP_NONE },
	{ "v2_pad", FLDT_UINT8X, OI(OFF(v2_pad)),
	  inode_core_v2_pad_count, FLD_ARRAY|FLD_COUNT|FLD_SKIPALL, TYP_NONE },
	{ "flushiter", FLDT_UINT16D, OI(COFF(flushiter)),
	  inode_core_flushiter_count, FLD_COUNT, TYP_NONE },
	/* -- */
	{ "atime", FLDT_TIMESTAMP, OI(COFF(atime)), C1, 0, TYP_NONE },
	{ "mtime", FLDT_TIMESTAMP, OI(COFF(mtime)), C1, 0, TYP_NONE },
	{ "ctime", FLDT_TIMESTAMP, OI(COFF(ctime)), C1, 0, TYP_NONE },
	{ "size", FLDT_FSIZE, OI(COFF(size)), C1, 0, TYP_NONE },
	{ "nblocks", FLDT_DRFSBNO, OI(COFF(nblocks)), C1, 0, TYP_NONE },
	{ "extsize", FLDT_EXTLEN, OI(COFF(extsize)), C1, 0, TYP_NONE },
	/* union 2 */
	{ "nextents", FLDT_UINT32D, inode_core_nextents_offset,
	  inode_core_nextents32_count, FLD_OFFSET|FLD_COUNT, TYP_NONE },
	{ "naextents", FLDT_UINT16D, inode_core_anextents_offset,
	  inode_core_anextents16_count, FLD_OFFSET|FLD_COUNT, TYP_NONE },
	{ "naextents", FLDT_UINT32D, inode_core_anextents_offset,
	  inode_core_anextents32_count, FLD_OFFSET|FLD_COUNT, TYP_NONE },
	{ "nrext64_pad", FLDT_UINT16D, OI(COFF(nrext64_pad)),
	  inode_core_nrext64_pad_count, FLD_COUNT|FLD_SKIPALL, TYP_NONE },
	/* -- */
	{ "forkoff", FLDT_UINT8D, OI(COFF(forkoff)), C1, 0, TYP_NONE },
	{ "aformat", FLDT_DINODE_FMT, OI(COFF(aformat)), C1, 0, TYP_NONE },
	{ "dmevmask", FLDT_UINT32X, OI(COFF(dmevmask)), C1, 0, TYP_NONE },
	{ "dmstate", FLDT_UINT16D, OI(COFF(dmstate)), C1, 0, TYP_NONE },
	{ "flags", FLDT_UINT16X, OI(COFF(flags)), C1, FLD_SKIPALL, TYP_NONE },
	{ "newrtbm", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_NEWRTBM_BIT - 1), C1,
	  0, TYP_NONE },
	{ "prealloc", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_PREALLOC_BIT - 1), C1,
	  0, TYP_NONE },
	{ "realtime", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_REALTIME_BIT - 1), C1,
	  0, TYP_NONE },
	{ "immutable", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_IMMUTABLE_BIT-1), C1,
	  0, TYP_NONE },
	{ "append", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_APPEND_BIT - 1), C1,
	  0, TYP_NONE },
	{ "sync", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_SYNC_BIT - 1), C1,
	  0, TYP_NONE },
	{ "noatime", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_NOATIME_BIT - 1), C1,
	  0, TYP_NONE },
	{ "nodump", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_NODUMP_BIT - 1), C1,
	  0, TYP_NONE },
	{ "rtinherit", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_RTINHERIT_BIT-1), C1,
	  0, TYP_NONE },
	{ "projinherit", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_PROJINHERIT_BIT-1), C1,
	  0, TYP_NONE },
	{ "nosymlinks", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_NOSYMLINKS_BIT-1), C1,
	  0, TYP_NONE },
	{ "extsz", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_EXTSIZE_BIT-1), C1,
	  0, TYP_NONE },
	{ "extszinherit", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_EXTSZINHERIT_BIT-1), C1,
	  0, TYP_NONE },
	{ "nodefrag", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_NODEFRAG_BIT-1), C1,
	  0, TYP_NONE },
	{ "filestream", FLDT_UINT1,
	  OI(COFF(flags) + bitsz(uint16_t) - XFS_DIFLAG_FILESTREAM_BIT-1), C1,
	  0, TYP_NONE },
	{ "gen", FLDT_UINT32D, OI(COFF(gen)), C1, 0, TYP_NONE },
	{ NULL }
};

const field_t	inode_v3_flds[] = {
	{ "crc", FLDT_CRC, OI(COFF(crc)), C1, 0, TYP_NONE },
	{ "change_count", FLDT_UINT64D, OI(COFF(changecount)), C1, 0, TYP_NONE },
	{ "lsn", FLDT_UINT64X, OI(COFF(lsn)), C1, 0, TYP_NONE },
	{ "flags2", FLDT_UINT64X, OI(COFF(flags2)), C1, 0, TYP_NONE },
	{ "cowextsize", FLDT_EXTLEN, OI(COFF(cowextsize)), C1, 0, TYP_NONE },
	{ "pad2", FLDT_UINT8X, OI(OFF(pad2)), CI(12), FLD_ARRAY|FLD_SKIPALL, TYP_NONE },
	{ "crtime", FLDT_TIMESTAMP, OI(COFF(crtime)), C1, 0, TYP_NONE },
	{ "inumber", FLDT_INO, OI(COFF(ino)), C1, 0, TYP_NONE },
	{ "uuid", FLDT_UUID, OI(COFF(uuid)), C1, 0, TYP_NONE },
	{ "reflink", FLDT_UINT1,
	  OI(COFF(flags2) + bitsz(uint64_t) - XFS_DIFLAG2_REFLINK_BIT-1), C1,
	  0, TYP_NONE },
	{ "cowextsz", FLDT_UINT1,
	  OI(COFF(flags2) + bitsz(uint64_t) - XFS_DIFLAG2_COWEXTSIZE_BIT-1), C1,
	  0, TYP_NONE },
	{ "dax", FLDT_UINT1,
	  OI(COFF(flags2) + bitsz(uint64_t) - XFS_DIFLAG2_DAX_BIT - 1), C1,
	  0, TYP_NONE },
	{ "bigtime", FLDT_UINT1,
	  OI(COFF(flags2) + bitsz(uint64_t) - XFS_DIFLAG2_BIGTIME_BIT - 1), C1,
	  0, TYP_NONE },
	{ "nrext64", FLDT_UINT1,
	  OI(COFF(flags2) + bitsz(uint64_t) - XFS_DIFLAG2_NREXT64_BIT - 1), C1,
	  0, TYP_NONE },
	{ "metadir", FLDT_UINT1,
	  OI(COFF(flags2) + bitsz(uint64_t) - XFS_DIFLAG2_METADIR_BIT-1), C1,
	  0, TYP_NONE },
	{ NULL }
};

const field_t	timestamp_flds[] = {
	{ "sec", FLDT_TIME, OI(0), C1, 0, TYP_NONE },
	{ "nsec", FLDT_NSEC, OI(0), C1, 0, TYP_NONE },
	{ NULL }
};

const field_t	inode_u_flds[] = {
	{ "bmbt", FLDT_BMROOTD, NULL, inode_u_bmbt_count, FLD_COUNT, TYP_NONE },
	{ "bmx", FLDT_BMAPBTDREC, NULL, inode_u_bmx_count, FLD_ARRAY|FLD_COUNT,
	  TYP_NONE },
	{ "c", FLDT_CHARNS, NULL, inode_u_c_count, FLD_COUNT, TYP_NONE },
	{ "dev", FLDT_DEV, NULL, inode_u_dev_count, FLD_COUNT, TYP_NONE },
	{ "muuid", FLDT_UUID, NULL, inode_u_muuid_count, FLD_COUNT, TYP_NONE },
	{ "sfdir2", FLDT_DIR2SF, NULL, inode_u_sfdir2_count, FLD_COUNT, TYP_NONE },
	{ "sfdir3", FLDT_DIR3SF, NULL, inode_u_sfdir3_count, FLD_COUNT, TYP_NONE },
	{ "symlink", FLDT_CHARNS, NULL, inode_u_symlink_count, FLD_COUNT,
	  TYP_NONE },
	{ "rtrmapbt", FLDT_RTRMAPROOT, NULL, inode_u_rtrmapbt_count, FLD_COUNT,
	  TYP_NONE },
	{ "rtrefcbt", FLDT_RTREFCROOT, NULL, inode_u_rtrefcbt_count, FLD_COUNT,
	  TYP_NONE },
	{ NULL }
};

const field_t	inode_a_flds[] = {
	{ "bmbt", FLDT_BMROOTA, NULL, inode_a_bmbt_count, FLD_COUNT, TYP_NONE },
	{ "bmx", FLDT_BMAPBTAREC, NULL, inode_a_bmx_count, FLD_ARRAY|FLD_COUNT,
	  TYP_NONE },
	{ "sfattr", FLDT_ATTRSHORT, NULL, inode_a_sfattr_count, FLD_COUNT,
	  TYP_NONE },
	{ NULL }
};

static const char	*dinode_fmt_name[] =
	{ "dev", "local", "extents", "btree", "uuid", "rmap", "refcount" };
static const int	dinode_fmt_name_size =
	sizeof(dinode_fmt_name) / sizeof(dinode_fmt_name[0]);

/*ARGSUSED*/
int
fp_dinode_fmt(
	void			*obj,
	int			bit,
	int			count,
	char			*fmtstr,
	int			size,
	int			arg,
	int			base,
	int			array)
{
	int			bitpos;
	enum xfs_dinode_fmt	f;
	int			i;

	for (i = 0, bitpos = bit; i < count; i++, bitpos += size) {
		f = (enum xfs_dinode_fmt)getbitval(obj, bitpos, size, BVUNSIGNED);
		if (array)
			dbprintf("%d:", i + base);
		if (f < 0 || f >= dinode_fmt_name_size)
			dbprintf("%d", (int)f);
		else
			dbprintf("%d (%s)", (int)f, dinode_fmt_name[(int)f]);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}

static int
inode_a_bmbt_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	if (!dip->di_forkoff)
		return 0;
	ASSERT((char *)XFS_DFORK_APTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_aformat == XFS_DINODE_FMT_BTREE;
}

static int
inode_a_bmx_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	if (!dip->di_forkoff)
		return 0;
	ASSERT((char *)XFS_DFORK_APTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_aformat == XFS_DINODE_FMT_EXTENTS ?
		xfs_dfork_attr_extents(dip) : 0;
}

static int
inode_a_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(startoff == 0);
	dip = obj;
	return dip->di_forkoff;
}

static int
inode_a_offset(
	void			*obj,
	int			startoff,
	int			idx)
{
	struct xfs_dinode	*dip;

	ASSERT(startoff == 0);
	ASSERT(idx == 0);
	dip = obj;
	ASSERT(dip->di_forkoff != 0);
	return bitize((int)((char *)XFS_DFORK_APTR(dip) - (char *)dip));
}

static int
inode_a_sfattr_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	if (!dip->di_forkoff)
		return 0;
	ASSERT((char *)XFS_DFORK_APTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_aformat == XFS_DINODE_FMT_LOCAL;
}

int
inode_a_size(
	void				*obj,
	int				startoff,
	int				idx)
{
	struct xfs_attr_shortform	*asf;
	struct xfs_dinode		*dip;

	ASSERT(startoff == 0);
	ASSERT(idx == 0);
	dip = obj;
	switch (dip->di_aformat) {
	case XFS_DINODE_FMT_LOCAL:
		asf = (struct xfs_attr_shortform *)XFS_DFORK_APTR(dip);
		return bitize(be16_to_cpu(asf->hdr.totsize));
	case XFS_DINODE_FMT_EXTENTS:
		return (int)xfs_dfork_attr_extents(dip) * bitsz(xfs_bmbt_rec_t);
	case XFS_DINODE_FMT_BTREE:
		return bitize((int)XFS_DFORK_ASIZE(dip, mp));
	default:
		return 0;
	}
}

static int
inode_core_nlinkv1_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;
	return dic->di_version == 1;
}

static int
inode_core_nlinkv2_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;
	return dic->di_version >= 2;
}

static int
inode_core_onlink_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;
	return dic->di_version >= 2;
}

static int
inode_core_projid_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;
	return dic->di_version >= 2;
}

static int
inode_core_v3_pad_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if ((dic->di_version == 3)
		&& !(dic->di_flags2 & cpu_to_be64(XFS_DIFLAG2_NREXT64)))
		return 1;

	return 0;
}

static int
inode_core_v2_pad_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (dic->di_version == 3)
		return 0;

	return 6;
}

static int
inode_core_flushiter_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (dic->di_version == 3)
		return 0;

	return 1;
}

static int
inode_core_nrext64_pad_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (xfs_dinode_has_large_extent_counts(dic))
		return 1;

	return 0;
}

static int
inode_core_nextents_offset(
	void			*obj,
	int			startoff,
	int			idx)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(idx == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (xfs_dinode_has_large_extent_counts(dic))
		return COFF(big_nextents);

	return COFF(nextents);
}

static int
inode_core_nextents32_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (xfs_dinode_has_large_extent_counts(dic))
		return 0;

	return 1;
}

static int
inode_core_nextents64_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (xfs_dinode_has_large_extent_counts(dic))
		return 1;

	return 0;
}

static int
inode_core_anextents_offset(
	void			*obj,
	int			startoff,
	int			idx)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(idx == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (xfs_dinode_has_large_extent_counts(dic))
		return COFF(big_anextents);

	return COFF(anextents);
}

static int
inode_core_anextents16_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (xfs_dinode_has_large_extent_counts(dic))
		return 0;

	return 1;
}

static int
inode_core_anextents32_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dic;

	ASSERT(startoff == 0);
	ASSERT(obj == iocur_top->data);
	dic = obj;

	if (xfs_dinode_has_large_extent_counts(dic))
		return 1;

	return 0;
}

static int
inode_f(
	int		argc,
	char		**argv)
{
	xfs_ino_t	ino;
	char		*p;

	if (argc > 1) {
		ino = strtoull(argv[1], &p, 0);
		if (*p != '\0') {
			dbprintf(_("bad value for inode number %s\n"), argv[1]);
			return 0;
		}
		set_cur_inode(ino);
	} else if (iocur_top->ino == NULLFSINO)
		dbprintf(_("no current inode\n"));
	else
		dbprintf(_("current inode number is %lld\n"), iocur_top->ino);
	return 0;
}

void
inode_init(void)
{
	add_command(&inode_cmd);
}

struct rtgroup_inodes {
	xfs_ino_t		rmap_ino;
};

static struct rtgroup_inodes	*rtgroup_inodes;
static struct bitmap		*rmap_inodes;
static struct bitmap		*refcount_inodes;

static inline int
set_rtgroup_rmap_inode(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_imeta_path	*path;
	struct xfs_trans	*tp;
	xfs_ino_t		rtino;
	int			error;

	if (!xfs_has_rtrmapbt(mp))
		return 0;

	error = -libxfs_rtrmapbt_create_path(mp, rgno, &path);
	if (error)
		return error;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		goto out_path;

	error = -libxfs_imeta_lookup(tp, path, &rtino);
	if (error)
		goto out_trans;

	if (rtino == NULLFSINO) {
		error = EFSCORRUPTED;
		goto out_trans;
	}

	error = bitmap_set(rmap_inodes, rtino, 1);
	if (error)
		goto out_trans;

	rtgroup_inodes[rgno].rmap_ino = rtino;

out_trans:
	libxfs_trans_cancel(tp);
out_path:
	libxfs_imeta_free_path(path);
	return error;
}

static inline int
set_rtgroup_refcount_inode(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	struct xfs_imeta_path	*path;
	struct xfs_trans	*tp;
	xfs_ino_t		rtino;
	int			error;

	if (!xfs_has_rtreflink(mp))
		return 0;

	error = -libxfs_rtrefcountbt_create_path(mp, rgno, &path);
	if (error)
		return error;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		goto out_path;

	error = -libxfs_imeta_lookup(tp, path, &rtino);
	if (error)
		goto out_trans;

	if (rtino == NULLFSINO) {
		error = EFSCORRUPTED;
		goto out_trans;
	}

	error = bitmap_set(refcount_inodes, rtino, 1);
out_trans:
	libxfs_trans_cancel(tp);
out_path:
	libxfs_imeta_free_path(path);
	return error;
}

int
init_rtmeta_inode_bitmaps(
	struct xfs_mount	*mp)
{
	xfs_rgnumber_t		rgno;
	int			error;

	if (rmap_inodes)
		return 0;

	rtgroup_inodes = calloc(mp->m_sb.sb_rgcount,
			sizeof(struct rtgroup_inodes));
	if (!rtgroup_inodes)
		return ENOMEM;

	error = bitmap_alloc(&rmap_inodes);
	if (error)
		return error;

	error = bitmap_alloc(&refcount_inodes);
	if (error)
		return error;

	for (rgno = 0; rgno < mp->m_sb.sb_rgcount; rgno++) {
		int err2 = set_rtgroup_rmap_inode(mp, rgno);
		if (err2 && !error)
			error = err2;
		err2 = set_rtgroup_refcount_inode(mp, rgno);
		if (err2 && !error)
			error = err2;
	}

	return error;
}

bool is_rtrmap_inode(xfs_ino_t ino)
{
	return bitmap_test(rmap_inodes, ino, 1);
}

xfs_rgnumber_t rtgroup_for_rtrmap_ino(struct xfs_mount *mp, xfs_ino_t ino)
{
	unsigned int i;

	for (i = 0; i < mp->m_sb.sb_rgcount; i++) {
		if (rtgroup_inodes[i].rmap_ino == ino)
			return i;
	}

	return NULLRGNUMBER;
}

bool is_rtrefcount_inode(xfs_ino_t ino)
{
	return bitmap_test(refcount_inodes, ino, 1);
}

typnm_t
inode_next_type(void)
{
	int		error;

	error = init_rtmeta_inode_bitmaps(mp);
	if (error) {
		dbprintf(_("error %d setting up rt metadata inode bitmaps\n"),
				error);
	}

	switch (iocur_top->mode & S_IFMT) {
	case S_IFDIR:
		return TYP_DIR2;
	case S_IFLNK:
		return TYP_SYMLINK;
	case S_IFREG:
		if (iocur_top->ino == mp->m_sb.sb_rbmino) {
			if (xfs_has_rtgroups(mp))
				return TYP_RGBITMAP;
			return TYP_RTBITMAP;
		} else if (iocur_top->ino == mp->m_sb.sb_rsumino) {
			if (xfs_has_rtgroups(mp))
				return TYP_RGSUMMARY;
			return TYP_RTSUMMARY;
		}
		else if (iocur_top->ino == mp->m_sb.sb_uquotino ||
			 iocur_top->ino == mp->m_sb.sb_gquotino ||
			 iocur_top->ino == mp->m_sb.sb_pquotino)
			return TYP_DQBLK;
		else if (is_rtrmap_inode(iocur_top->ino))
			return TYP_RTRMAPBT;
		else if (is_rtrefcount_inode(iocur_top->ino))
			return TYP_RTREFCBT;

		return TYP_DATA;
	default:
		return TYP_NONE;
	}
}

int
inode_size(
	void	*obj,
	int	startoff,
	int	idx)
{
	return bitize(mp->m_sb.sb_inodesize);
}

static int
inode_u_offset(
	void			*obj,
	int			startoff,
	int			idx)
{
	struct xfs_dinode	*dip;

	ASSERT(startoff == 0);
	ASSERT(idx == 0);
	dip = obj;
	return bitize((int)((char *)XFS_DFORK_DPTR(dip) - (char *)dip));
}

static int
inode_u_bmbt_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_BTREE;
}

static int
inode_u_bmx_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_EXTENTS ?
		xfs_dfork_data_extents(dip) : 0;
}

static int
inode_u_c_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_LOCAL &&
	       (be16_to_cpu(dip->di_mode) & S_IFMT) == S_IFREG ?
		(int)be64_to_cpu(dip->di_size) : 0;
}

static int
inode_u_dev_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_DEV;
}

static int
inode_u_muuid_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_UUID;
}

static int
inode_u_sfdir2_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_LOCAL &&
	       (be16_to_cpu(dip->di_mode) & S_IFMT) == S_IFDIR &&
	       !xfs_has_ftype(mp);
}

static int
inode_u_sfdir3_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_LOCAL &&
	       (be16_to_cpu(dip->di_mode) & S_IFMT) == S_IFDIR &&
	       xfs_has_ftype(mp);
}

static int
inode_u_rtrmapbt_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_RMAP;
}

static int
inode_u_rtrefcbt_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_REFCOUNT;
}

int
inode_u_size(
	void			*obj,
	int			startoff,
	int			idx)
{
	struct xfs_dinode	*dip;

	ASSERT(startoff == 0);
	ASSERT(idx == 0);
	dip = obj;
	switch (dip->di_format) {
	case XFS_DINODE_FMT_DEV:
		return bitsz(xfs_dev_t);
	case XFS_DINODE_FMT_LOCAL:
		return bitize((int)be64_to_cpu(dip->di_size));
	case XFS_DINODE_FMT_EXTENTS:
		return (int)xfs_dfork_data_extents(dip) * bitsz(xfs_bmbt_rec_t);
	case XFS_DINODE_FMT_BTREE:
		return bitize((int)XFS_DFORK_DSIZE(dip, mp));
	case XFS_DINODE_FMT_UUID:
		return bitsz(uuid_t);
	default:
		return 0;
	}
}

static int
inode_u_symlink_count(
	void			*obj,
	int			startoff)
{
	struct xfs_dinode	*dip;

	ASSERT(bitoffs(startoff) == 0);
	ASSERT(obj == iocur_top->data);
	dip = obj;
	ASSERT((char *)XFS_DFORK_DPTR(dip) - (char *)dip == byteize(startoff));
	return dip->di_format == XFS_DINODE_FMT_LOCAL &&
	       (be16_to_cpu(dip->di_mode) & S_IFMT) == S_IFLNK ?
		(int)be64_to_cpu(dip->di_size) : 0;
}

/*
 * We are now using libxfs for our IO backend, so we should always try to use
 * inode cluster buffers rather than filesystem block sized buffers for reading
 * inodes. This means that we always use the same buffers as libxfs operations
 * does, and that avoids buffer cache issues caused by overlapping buffers. This
 * can be seen clearly when trying to read the root inode. Much of this logic is
 * similar to libxfs_imap().
 */
void
set_cur_inode(
	xfs_ino_t		ino)
{
	xfs_agblock_t		agbno;
	xfs_agino_t		agino;
	xfs_agnumber_t		agno;
	struct xfs_dinode	*dip;
	int			offset;
	int			numblks = blkbb;
	xfs_agblock_t		cluster_agbno;
	struct xfs_ino_geometry	*igeo = M_IGEO(mp);


	agno = XFS_INO_TO_AGNO(mp, ino);
	agino = XFS_INO_TO_AGINO(mp, ino);
	agbno = XFS_AGINO_TO_AGBNO(mp, agino);
	offset = XFS_AGINO_TO_OFFSET(mp, agino);
	if (agno >= mp->m_sb.sb_agcount || agbno >= mp->m_sb.sb_agblocks ||
	    offset >= mp->m_sb.sb_inopblock ||
	    XFS_AGINO_TO_INO(mp, agno, agino) != ino) {
		dbprintf(_("bad inode number %lld\n"), ino);
		return;
	}
	cur_agno = agno;

	if (igeo->inode_cluster_size > mp->m_sb.sb_blocksize &&
	    igeo->inoalign_mask) {
		xfs_agblock_t	chunk_agbno;
		xfs_agblock_t	offset_agbno;

		offset_agbno = agbno & igeo->inoalign_mask;
		chunk_agbno = agbno - offset_agbno;
		cluster_agbno = chunk_agbno +
			((offset_agbno / M_IGEO(mp)->blocks_per_cluster) *
			 M_IGEO(mp)->blocks_per_cluster);
		offset += ((agbno - cluster_agbno) * mp->m_sb.sb_inopblock);
		numblks = XFS_FSB_TO_BB(mp, M_IGEO(mp)->blocks_per_cluster);
	} else
		cluster_agbno = agbno;

	/*
	 * First set_cur to the block with the inode
	 * then use off_cur to get the right part of the buffer.
	 */
	ASSERT(typtab[TYP_INODE].typnm == TYP_INODE);

	/* ingore ring update here, do it explicitly below */
	set_cur(&typtab[TYP_INODE], XFS_AGB_TO_DADDR(mp, agno, cluster_agbno),
		numblks, DB_RING_IGN, NULL);
	off_cur(offset << mp->m_sb.sb_inodelog, mp->m_sb.sb_inodesize);
	if (!iocur_top->data)
		return;
	dip = iocur_top->data;
	iocur_top->ino_buf = 1;
	iocur_top->ino = ino;
	iocur_top->mode = be16_to_cpu(dip->di_mode);
	if ((iocur_top->mode & S_IFMT) == S_IFDIR)
		iocur_top->dirino = ino;

	if (xfs_has_crc(mp)) {
		iocur_top->ino_crc_ok = libxfs_verify_cksum((char *)dip,
						    mp->m_sb.sb_inodesize,
						    XFS_DINODE_CRC_OFF);
		if (!iocur_top->ino_crc_ok)
			dbprintf(
_("Metadata CRC error detected for ino %lld\n"),
				ino);
	}

	/* track updated info in ring */
	ring_add();
}

void
xfs_inode_set_crc(
	struct xfs_buf *bp)
{
	ASSERT(iocur_top->ino_buf);
	ASSERT(iocur_top->bp == bp);

	libxfs_dinode_calc_crc(mp, iocur_top->data);
	iocur_top->ino_crc_ok = 1;
}
