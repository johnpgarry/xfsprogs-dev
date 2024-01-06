// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * Copyright (c) 2016 Oracle, Inc.
 * All Rights Reserved.
 */
#include "libxfs.h"
#include "libxlog.h"

#include "logprint.h"

/* Extent Free Items */

static int
xfs_efi_copy_format(
	char			  *buf,
	uint			  len,
	struct xfs_efi_log_format *dst_efi_fmt,
	int			  continued)
{
	uint i;
	uint nextents = ((xfs_efi_log_format_t *)buf)->efi_nextents;
	uint dst_len = xfs_efi_log_format_sizeof(nextents);
	uint len32 = xfs_efi_log_format32_sizeof(nextents);
	uint len64 = xfs_efi_log_format64_sizeof(nextents);

	if (len == dst_len || continued) {
		memcpy((char *)dst_efi_fmt, buf, len);
		return 0;
	} else if (len == len32) {
		xfs_efi_log_format_32_t *src_efi_fmt_32 = (xfs_efi_log_format_32_t *)buf;

		dst_efi_fmt->efi_type	 = src_efi_fmt_32->efi_type;
		dst_efi_fmt->efi_size	 = src_efi_fmt_32->efi_size;
		dst_efi_fmt->efi_nextents = src_efi_fmt_32->efi_nextents;
		dst_efi_fmt->efi_id	   = src_efi_fmt_32->efi_id;
		for (i = 0; i < dst_efi_fmt->efi_nextents; i++) {
			dst_efi_fmt->efi_extents[i].ext_start =
				src_efi_fmt_32->efi_extents[i].ext_start;
			dst_efi_fmt->efi_extents[i].ext_len =
				src_efi_fmt_32->efi_extents[i].ext_len;
		}
		return 0;
	} else if (len == len64) {
		xfs_efi_log_format_64_t *src_efi_fmt_64 = (xfs_efi_log_format_64_t *)buf;

		dst_efi_fmt->efi_type	 = src_efi_fmt_64->efi_type;
		dst_efi_fmt->efi_size	 = src_efi_fmt_64->efi_size;
		dst_efi_fmt->efi_nextents = src_efi_fmt_64->efi_nextents;
		dst_efi_fmt->efi_id	   = src_efi_fmt_64->efi_id;
		for (i = 0; i < dst_efi_fmt->efi_nextents; i++) {
			dst_efi_fmt->efi_extents[i].ext_start =
				src_efi_fmt_64->efi_extents[i].ext_start;
			dst_efi_fmt->efi_extents[i].ext_len =
				src_efi_fmt_64->efi_extents[i].ext_len;
		}
		return 0;
	}
	fprintf(stderr, _("%s: bad size of efi format: %u; expected %u or %u; nextents = %u\n"),
		progname, len, len32, len64, nextents);
	return 1;
}

int
xlog_print_trans_efi(
	char			**ptr,
	uint			src_len,
	int			continued)
{
	const char		*item_name = "EFI?";
	xfs_efi_log_format_t	*src_f, *f = NULL;
	uint			dst_len;
	xfs_extent_t		*ex;
	int			i;
	int			error = 0;
	int			core_size = offsetof(xfs_efi_log_format_t, efi_extents);

	/*
	 * memmove to ensure 8-byte alignment for the long longs in
	 * xfs_efi_log_format_t structure
	 */
	if ((src_f = (xfs_efi_log_format_t *)malloc(src_len)) == NULL) {
		fprintf(stderr, _("%s: xlog_print_trans_efi: malloc failed\n"), progname);
		exit(1);
	}
	memmove((char*)src_f, *ptr, src_len);
	*ptr += src_len;

	/* convert to native format */
	dst_len = xfs_efi_log_format_sizeof(src_f->efi_nextents);

	if (continued && src_len < core_size) {
		printf(_("EFI: Not enough data to decode further\n"));
		error = 1;
		goto error;
	}

	if ((f = (xfs_efi_log_format_t *)malloc(dst_len)) == NULL) {
		fprintf(stderr, _("%s: xlog_print_trans_efi: malloc failed\n"), progname);
		exit(1);
	}
	if (xfs_efi_copy_format((char*)src_f, src_len, f, continued)) {
		error = 1;
		goto error;
	}

	switch (f->efi_type) {
	case XFS_LI_EFI:	item_name = "EFI"; break;
	case XFS_LI_EFI_RT:	item_name = "EFI_RT"; break;
	}

	printf(_("%s:  #regs: %d	num_extents: %u  id: 0x%llx\n"),
			item_name, f->efi_size, f->efi_nextents,
			(unsigned long long)f->efi_id);

	if (continued) {
		printf(_("EFI free extent data skipped (CONTINUE set, no space)\n"));
		goto error;
	}

	ex = f->efi_extents;
	for (i=0; i < f->efi_nextents; i++) {
		printf("(s: 0x%llx, l: %u) ",
			(unsigned long long)ex->ext_start, ex->ext_len);
		if (i % 4 == 3) printf("\n");
		ex++;
	}
	if (i % 4 != 0)
		printf("\n");
error:
	free(src_f);
	free(f);
	return error;
}	/* xlog_print_trans_efi */

void
xlog_recover_print_efi(
	struct xlog_recover_item *item)
{
	const char		*item_name = "EFI?";
	xfs_efi_log_format_t	*f, *src_f;
	xfs_extent_t		*ex;
	int			i;
	uint			src_len, dst_len;

	src_f = (xfs_efi_log_format_t *)item->ri_buf[0].i_addr;
	src_len = item->ri_buf[0].i_len;
	/*
	 * An xfs_efi_log_format structure contains a variable length array
	 * as the last field.
	 * Each element is of size xfs_extent_32_t or xfs_extent_64_t.
	 * Need to convert to native format.
	 */
	dst_len = sizeof(xfs_efi_log_format_t) +
		(src_f->efi_nextents) * sizeof(xfs_extent_t);
	if ((f = (xfs_efi_log_format_t *)malloc(dst_len)) == NULL) {
		fprintf(stderr, _("%s: xlog_recover_print_efi: malloc failed\n"),
			progname);
		exit(1);
	}
	if (xfs_efi_copy_format((char*)src_f, src_len, f, 0)) {
		free(f);
		return;
	}

	switch (f->efi_type) {
	case XFS_LI_EFI:	item_name = "EFI"; break;
	case XFS_LI_EFI_RT:	item_name = "EFI_RT"; break;
	}

	printf(_("	%s:  #regs:%d	num_extents:%u  id:0x%llx\n"),
			item_name, f->efi_size, f->efi_nextents,
			(unsigned long long)f->efi_id);
	ex = f->efi_extents;
	printf("	");
	for (i=0; i< f->efi_nextents; i++) {
		printf("(s: 0x%llx, l: %u) ",
			(unsigned long long)ex->ext_start, ex->ext_len);
		if (i % 4 == 3)
			printf("\n");
		ex++;
	}
	if (i % 4 != 0)
		printf("\n");
	free(f);
}

int
xlog_print_trans_efd(char **ptr, uint len)
{
	const char		*item_name = "EFD?";
	xfs_efd_log_format_t	*f;
	xfs_efd_log_format_t	lbuf;

	/* size without extents at end */
	uint core_size = sizeof(xfs_efd_log_format_t);

	/*
	 * memmove to ensure 8-byte alignment for the long longs in
	 * xfs_efd_log_format_t structure
	 */
	memmove(&lbuf, *ptr, min(core_size, len));
	f = &lbuf;

	switch (f->efd_type) {
	case XFS_LI_EFD:	item_name = "EFD"; break;
	case XFS_LI_EFD_RT:	item_name = "EFD_RT"; break;
	}

	*ptr += len;
	if (len >= core_size) {
		printf(_("%s:  #regs: %d	num_extents: %d  id: 0x%llx\n"),
				item_name, f->efd_size, f->efd_nextents,
				(unsigned long long)f->efd_efi_id);

		/* don't print extents as they are not used */

		return 0;
	} else {
		printf(_("EFD: Not enough data to decode further\n"));
	return 1;
	}
}	/* xlog_print_trans_efd */

void
xlog_recover_print_efd(
	struct xlog_recover_item *item)
{
	const char		*item_name = "EFD?";
	xfs_efd_log_format_t	*f;

	f = (xfs_efd_log_format_t *)item->ri_buf[0].i_addr;

	switch (f->efd_type) {
	case XFS_LI_EFD:	item_name = "EFD"; break;
	case XFS_LI_EFD_RT:	item_name = "EFD_RT"; break;
	}

	/*
	 * An xfs_efd_log_format structure contains a variable length array
	 * as the last field.
	 * Each element is of size xfs_extent_32_t or xfs_extent_64_t.
	 * However, the extents are never used and won't be printed.
	 */
	printf(_("	%s:  #regs: %d	num_extents: %d  id: 0x%llx\n"),
			item_name, f->efd_size, f->efd_nextents,
			(unsigned long long)f->efd_efi_id);
}

/* Reverse Mapping Update Items */

static int
xfs_rui_copy_format(
	char			  *buf,
	uint			  len,
	struct xfs_rui_log_format *dst_fmt,
	int			  continued)
{
	uint nextents = ((struct xfs_rui_log_format *)buf)->rui_nextents;
	uint dst_len = xfs_rui_log_format_sizeof(nextents);

	if (len == dst_len || continued) {
		memcpy((char *)dst_fmt, buf, len);
		return 0;
	}
	fprintf(stderr, _("%s: bad size of RUI format: %u; expected %u; nextents = %u\n"),
		progname, len, dst_len, nextents);
	return 1;
}

int
xlog_print_trans_rui(
	char			**ptr,
	uint			src_len,
	int			continued)
{
	const char		*item_name = "RUI?";
	struct xfs_rui_log_format	*src_f, *f = NULL;
	uint			dst_len;
	uint			nextents;
	struct xfs_map_extent	*ex;
	int			i;
	int			error = 0;
	int			core_size;

	core_size = offsetof(struct xfs_rui_log_format, rui_extents);

	/*
	 * memmove to ensure 8-byte alignment for the long longs in
	 * struct xfs_rui_log_format structure
	 */
	src_f = malloc(src_len);
	if (src_f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	memmove((char*)src_f, *ptr, src_len);
	*ptr += src_len;

	/* convert to native format */
	nextents = src_f->rui_nextents;
	dst_len = xfs_rui_log_format_sizeof(nextents);

	if (continued && src_len < core_size) {
		printf(_("RUI: Not enough data to decode further\n"));
		error = 1;
		goto error;
	}

	f = malloc(dst_len);
	if (f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	if (xfs_rui_copy_format((char *)src_f, src_len, f, continued)) {
		error = 1;
		goto error;
	}

	switch (f->rui_type) {
	case XFS_LI_RUI:	item_name = "RUI"; break;
	case XFS_LI_RUI_RT:	item_name = "RUI_RT"; break;
	}

	printf(_("%s:  #regs: %d	num_extents: %d  id: 0x%llx\n"),
			item_name, f->rui_size, f->rui_nextents,
			(unsigned long long)f->rui_id);

	if (continued) {
		printf(_("RUI extent data skipped (CONTINUE set, no space)\n"));
		goto error;
	}

	ex = f->rui_extents;
	for (i=0; i < f->rui_nextents; i++) {
		printf("(s: 0x%llx, l: %d, own: %lld, off: %llu, f: 0x%x) ",
			(unsigned long long)ex->me_startblock, ex->me_len,
			(long long)ex->me_owner,
			(unsigned long long)ex->me_startoff, ex->me_flags);
		printf("\n");
		ex++;
	}
error:
	free(src_f);
	free(f);
	return error;
}

void
xlog_recover_print_rui(
	struct xlog_recover_item	*item)
{
	char				*src_f;
	uint				src_len;

	src_f = item->ri_buf[0].i_addr;
	src_len = item->ri_buf[0].i_len;

	xlog_print_trans_rui(&src_f, src_len, 0);
}

int
xlog_print_trans_rud(
	char				**ptr,
	uint				len)
{
	const char			*item_name = "RUD?";
	struct xfs_rud_log_format	*f;
	struct xfs_rud_log_format	lbuf;

	/* size without extents at end */
	uint core_size = sizeof(struct xfs_rud_log_format);

	/*
	 * memmove to ensure 8-byte alignment for the long longs in
	 * xfs_efd_log_format_t structure
	 */
	memmove(&lbuf, *ptr, min(core_size, len));
	f = &lbuf;

	switch (f->rud_type) {
	case XFS_LI_RUD:	item_name = "RUD"; break;
	case XFS_LI_RUD_RT:	item_name = "RUD_RT"; break;
	}

	*ptr += len;
	if (len >= core_size) {
		printf(_("%s:  #regs: %d	                 id: 0x%llx\n"),
				item_name, f->rud_size,
				(unsigned long long)f->rud_rui_id);

		/* don't print extents as they are not used */

		return 0;
	} else {
		printf(_("RUD: Not enough data to decode further\n"));
		return 1;
	}
}

void
xlog_recover_print_rud(
	struct xlog_recover_item	*item)
{
	char				*f;

	f = item->ri_buf[0].i_addr;
	xlog_print_trans_rud(&f, sizeof(struct xfs_rud_log_format));
}

/* Reference Count Update Items */

static int
xfs_cui_copy_format(
	struct xfs_cui_log_format *cui,
	uint			  len,
	struct xfs_cui_log_format *dst_fmt,
	int			  continued)
{
	uint nextents;
	uint dst_len;

	nextents = cui->cui_nextents;
	dst_len = xfs_cui_log_format_sizeof(nextents);

	if (len == dst_len || continued) {
		memcpy(dst_fmt, cui, len);
		return 0;
	}
	fprintf(stderr, _("%s: bad size of CUI format: %u; expected %u; nextents = %u\n"),
		progname, len, dst_len, nextents);
	return 1;
}

int
xlog_print_trans_cui(
	char			**ptr,
	uint			src_len,
	int			continued)
{
	const char		*item_name = "CUI?";
	struct xfs_cui_log_format	*src_f, *f = NULL;
	uint			dst_len;
	uint			nextents;
	struct xfs_phys_extent	*ex;
	int			i;
	int			error = 0;
	int			core_size;

	core_size = offsetof(struct xfs_cui_log_format, cui_extents);

	src_f = malloc(src_len);
	if (src_f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	memcpy(src_f, *ptr, src_len);
	*ptr += src_len;

	/* convert to native format */
	nextents = src_f->cui_nextents;
	dst_len = xfs_cui_log_format_sizeof(nextents);

	if (continued && src_len < core_size) {
		printf(_("CUI: Not enough data to decode further\n"));
		error = 1;
		goto error;
	}

	f = malloc(dst_len);
	if (f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	if (xfs_cui_copy_format(src_f, src_len, f, continued)) {
		error = 1;
		goto error;
	}

	switch (f->cui_type) {
	case XFS_LI_CUI:	item_name = "CUI"; break;
	case XFS_LI_CUI_RT:	item_name = "CUI_RT"; break;
	}

	printf(_("%s:  #regs: %d	num_extents: %d  id: 0x%llx\n"),
			item_name, f->cui_size, f->cui_nextents,
			(unsigned long long)f->cui_id);

	if (continued) {
		printf(_("CUI extent data skipped (CONTINUE set, no space)\n"));
		goto error;
	}

	ex = f->cui_extents;
	for (i=0; i < f->cui_nextents; i++) {
		printf("(s: 0x%llx, l: %d, f: 0x%x) ",
			(unsigned long long)ex->pe_startblock, ex->pe_len,
			ex->pe_flags);
		printf("\n");
		ex++;
	}
error:
	free(src_f);
	free(f);
	return error;
}

void
xlog_recover_print_cui(
	struct xlog_recover_item	*item)
{
	char				*src_f;
	uint				src_len;

	src_f = item->ri_buf[0].i_addr;
	src_len = item->ri_buf[0].i_len;

	xlog_print_trans_cui(&src_f, src_len, 0);
}

int
xlog_print_trans_cud(
	char				**ptr,
	uint				len)
{
	const char			*item_name = "CUD?";
	struct xfs_cud_log_format	*f;
	struct xfs_cud_log_format	lbuf;

	/* size without extents at end */
	uint core_size = sizeof(struct xfs_cud_log_format);

	memcpy(&lbuf, *ptr, min(core_size, len));
	f = &lbuf;

	switch (f->cud_type) {
	case XFS_LI_CUD:	item_name = "CUD"; break;
	case XFS_LI_CUD_RT:	item_name = "CUD_RT"; break;
	}

	*ptr += len;
	if (len >= core_size) {
		printf(_("%s:  #regs: %d	                 id: 0x%llx\n"),
				item_name, f->cud_size,
				(unsigned long long)f->cud_cui_id);

		/* don't print extents as they are not used */

		return 0;
	} else {
		printf(_("CUD: Not enough data to decode further\n"));
		return 1;
	}
}

void
xlog_recover_print_cud(
	struct xlog_recover_item	*item)
{
	char				*f;

	f = item->ri_buf[0].i_addr;
	xlog_print_trans_cud(&f, sizeof(struct xfs_cud_log_format));
}

/* Block Mapping Update Items */

static int
xfs_bui_copy_format(
	struct xfs_bui_log_format *bui,
	uint			  len,
	struct xfs_bui_log_format *dst_fmt,
	int			  continued)
{
	uint nextents;
	uint dst_len;

	nextents = bui->bui_nextents;
	dst_len = xfs_bui_log_format_sizeof(nextents);

	if (len == dst_len || continued) {
		memcpy(dst_fmt, bui, len);
		return 0;
	}
	fprintf(stderr, _("%s: bad size of BUI format: %u; expected %u; nextents = %u\n"),
		progname, len, dst_len, nextents);
	return 1;
}

int
xlog_print_trans_bui(
	char			**ptr,
	uint			src_len,
	int			continued)
{
	struct xfs_bui_log_format	*src_f, *f = NULL;
	uint			dst_len;
	uint			nextents;
	struct xfs_map_extent	*ex;
	int			i;
	int			error = 0;
	int			core_size;

	core_size = offsetof(struct xfs_bui_log_format, bui_extents);

	src_f = malloc(src_len);
	if (src_f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	memcpy(src_f, *ptr, src_len);
	*ptr += src_len;

	/* convert to native format */
	nextents = src_f->bui_nextents;
	dst_len = xfs_bui_log_format_sizeof(nextents);

	if (continued && src_len < core_size) {
		printf(_("BUI: Not enough data to decode further\n"));
		error = 1;
		goto error;
	}

	f = malloc(dst_len);
	if (f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	if (xfs_bui_copy_format(src_f, src_len, f, continued)) {
		error = 1;
		goto error;
	}

	printf(_("BUI:  #regs: %d	num_extents: %d  id: 0x%llx\n"),
		f->bui_size, f->bui_nextents, (unsigned long long)f->bui_id);

	if (continued) {
		printf(_("BUI extent data skipped (CONTINUE set, no space)\n"));
		goto error;
	}

	ex = f->bui_extents;
	for (i=0; i < f->bui_nextents; i++) {
		printf("(s: 0x%llx, l: %d, own: %lld, off: %llu, f: 0x%x) ",
			(unsigned long long)ex->me_startblock, ex->me_len,
			(long long)ex->me_owner,
			(unsigned long long)ex->me_startoff, ex->me_flags);
		printf("\n");
		ex++;
	}
error:
	free(src_f);
	free(f);
	return error;
}

void
xlog_recover_print_bui(
	struct xlog_recover_item	*item)
{
	char				*src_f;
	uint				src_len;

	src_f = item->ri_buf[0].i_addr;
	src_len = item->ri_buf[0].i_len;

	xlog_print_trans_bui(&src_f, src_len, 0);
}

int
xlog_print_trans_bud(
	char				**ptr,
	uint				len)
{
	struct xfs_bud_log_format	*f;
	struct xfs_bud_log_format	lbuf;

	/* size without extents at end */
	uint core_size = sizeof(struct xfs_bud_log_format);

	memcpy(&lbuf, *ptr, min(core_size, len));
	f = &lbuf;
	*ptr += len;
	if (len >= core_size) {
		printf(_("BUD:  #regs: %d	                 id: 0x%llx\n"),
			f->bud_size,
			(unsigned long long)f->bud_bui_id);

		/* don't print extents as they are not used */

		return 0;
	} else {
		printf(_("BUD: Not enough data to decode further\n"));
		return 1;
	}
}

void
xlog_recover_print_bud(
	struct xlog_recover_item	*item)
{
	char				*f;

	f = item->ri_buf[0].i_addr;
	xlog_print_trans_bud(&f, sizeof(struct xfs_bud_log_format));
}

/* Attr Items */

static int
xfs_attri_copy_log_format(
	char				*buf,
	uint				len,
	struct xfs_attri_log_format	*dst_attri_fmt)
{
	uint dst_len = sizeof(struct xfs_attri_log_format);

	if (len == dst_len) {
		memcpy((char *)dst_attri_fmt, buf, len);
		return 0;
	}

	fprintf(stderr, _("%s: bad size of attri format: %u; expected %u\n"),
		progname, len, dst_len);
	return 1;
}

static void
dump_pptr(
	const char			*tag,
	const void			*name_ptr,
	unsigned int			name_len,
	const void			*value_ptr,
	unsigned int			value_len)
{
	struct xfs_parent_name_irec	irec;

	if (name_len < sizeof(struct xfs_parent_name_rec)) {
		printf("PPTR: %s CORRUPT\n", tag);
		return;
	}

	libxfs_parent_irec_from_disk(&irec, name_ptr, value_ptr, value_len);

	printf("PPTR: %s attr_namelen %u attr_valuelen %u\n", tag, name_len, value_len);
	printf("PPTR: %s parent_ino %llu parent_gen %u namehash 0x%x namelen %u name '%.*s'\n",
			tag,
			(unsigned long long)irec.p_ino,
			irec.p_gen,
			irec.p_namehash,
			irec.p_namelen,
			irec.p_namelen,
			irec.p_name);
}

static void
dump_pptr_update(
	const void	*name_ptr,
	unsigned int	name_len,
	const void	*new_name_ptr,
	unsigned int	new_name_len,
	const void	*value_ptr,
	unsigned int	value_len,
	const void	*new_value_ptr,
	unsigned int	new_value_len)
{
	if (new_name_ptr && name_ptr) {
		dump_pptr("OLDNAME", name_ptr, name_len, value_ptr, value_len);
		dump_pptr("NEWNAME", new_name_ptr, new_name_len, new_value_ptr,
				new_value_len);
		return;
	}

	if (name_ptr)
		dump_pptr("NAME", name_ptr, name_len, value_ptr, value_len);
	if (new_name_ptr)
		dump_pptr("NEWNAME", new_name_ptr, new_name_len, new_value_ptr,
				new_value_len);
}

static inline unsigned int
xfs_attr_log_item_op(const struct xfs_attri_log_format *attrp)
{
	return attrp->alfi_op_flags & XFS_ATTRI_OP_FLAGS_TYPE_MASK;
}

int
xlog_print_trans_attri(
	char				**ptr,
	uint				src_len,
	int				*i)
{
	struct xfs_attri_log_format	*src_f = NULL;
	xlog_op_header_t		*head = NULL;
	void				*name_ptr = NULL;
	void				*new_name_ptr = NULL;
	void				*value_ptr = NULL;
	void				*new_value_ptr = NULL;
	uint				dst_len;
	unsigned int			name_len = 0;
	unsigned int			new_name_len = 0;
	unsigned int			value_len = 0;
	unsigned int			new_value_len = 0;
	int				error = 0;

	dst_len = sizeof(struct xfs_attri_log_format);
	if (src_len != dst_len) {
		fprintf(stderr, _("%s: bad size of attri format: %u; expected %u\n"),
				progname, src_len, dst_len);
		return 1;
	}

	/*
	 * memmove to ensure 8-byte alignment for the long longs in
	 * xfs_attri_log_format_t structure
	 */
	src_f = malloc(src_len);
	if (!src_f) {
		fprintf(stderr, _("%s: xlog_print_trans_attri: malloc failed\n"),
				progname);
		exit(1);
	}
	memmove((char*)src_f, *ptr, src_len);
	*ptr += src_len;

	if (xfs_attr_log_item_op(src_f) == XFS_ATTRI_OP_FLAGS_NVREPLACE) {
		name_len      = src_f->alfi_old_name_len;
		new_name_len  = src_f->alfi_new_name_len;
		value_len     = src_f->alfi_value_len;
		new_value_len = src_f->alfi_new_value_len;
	} else {
		name_len      = src_f->alfi_name_len;
		value_len     = src_f->alfi_value_len;
	}

	printf(_("ATTRI:  #regs: %d	f: 0x%x, ino: 0x%llx, attr_filter: 0x%x, name_len: %u, new_name_len: %u, value_len: %u, new_value_len: %u  id: 0x%llx\n"),
			src_f->alfi_size,
			src_f->alfi_op_flags,
			(unsigned long long)src_f->alfi_ino,
			src_f->alfi_attr_filter,
			name_len,
			new_name_len,
			value_len,
			new_value_len,
			(unsigned long long)src_f->alfi_id);

	if (name_len > 0) {
		printf(_("\n"));
		(*i)++;
		head = (xlog_op_header_t *)*ptr;
		xlog_print_op_header(head, *i, ptr);
		name_ptr = *ptr;
		error = xlog_print_trans_attri_name(ptr,
				be32_to_cpu(head->oh_len), "name");
		if (error)
			goto error;
	}

	if (new_name_len > 0) {
		printf(_("\n"));
		(*i)++;
		head = (xlog_op_header_t *)*ptr;
		xlog_print_op_header(head, *i, ptr);
		new_name_ptr = *ptr;
		error = xlog_print_trans_attri_name(ptr,
				be32_to_cpu(head->oh_len), "newname");
		if (error)
			goto error;
	}

	if (value_len > 0) {
		printf(_("\n"));
		(*i)++;
		head = (xlog_op_header_t *)*ptr;
		xlog_print_op_header(head, *i, ptr);
		value_ptr = *ptr;
		error = xlog_print_trans_attri_value(ptr,
				be32_to_cpu(head->oh_len), value_len, "value");
		if (error)
			goto error;
	}

	if (new_value_len > 0) {
		printf(_("\n"));
		(*i)++;
		head = (xlog_op_header_t *)*ptr;
		xlog_print_op_header(head, *i, ptr);
		new_value_ptr = *ptr;
		error = xlog_print_trans_attri_value(ptr,
				be32_to_cpu(head->oh_len), new_value_len,
				"newvalue");
		if (error)
			goto error;
	}

	if (src_f->alfi_attr_filter & XFS_ATTR_PARENT)
		dump_pptr_update(name_ptr, name_len,
				 new_name_ptr, new_name_len,
				 value_ptr, value_len,
				 new_value_ptr, new_value_len);
error:
	free(src_f);

	return error;
}	/* xlog_print_trans_attri */

int
xlog_print_trans_attri_name(
	char				**ptr,
	uint				src_len,
	const char			*tag)
{
	printf(_("ATTRI:  %s len:%u\n"), tag, src_len);
	print_or_dump(*ptr, src_len);

	*ptr += src_len;

	return 0;
}

int
xlog_print_trans_attri_value(
	char				**ptr,
	uint				src_len,
	int				value_len,
	const char			*tag)
{
	int len = min(value_len, src_len);

	printf(_("ATTRI:  %s len:%u\n"), tag, value_len);
	print_or_dump(*ptr, len);

	*ptr += src_len;

	return 0;
}

void
xlog_recover_print_attri(
	struct xlog_recover_item	*item)
{
	struct xfs_attri_log_format	*f, *src_f = NULL;
	void				*name_ptr = NULL;
	void				*new_name_ptr = NULL;
	void				*value_ptr = NULL;
	void				*new_value_ptr = NULL;
	uint				src_len, dst_len;
	unsigned int			name_len = 0;
	unsigned int			new_name_len = 0;
	unsigned int			value_len = 0;
	unsigned int			new_value_len = 0;
	int				region = 0;

	src_f = (struct xfs_attri_log_format *)item->ri_buf[0].i_addr;
	src_len = item->ri_buf[region].i_len;

	/*
	 * An xfs_attri_log_format structure contains a attribute name and
	 * variable length value  as the last field.
	 */
	dst_len = sizeof(struct xfs_attri_log_format);

	if ((f = ((struct xfs_attri_log_format *)malloc(dst_len))) == NULL) {
		fprintf(stderr, _("%s: xlog_recover_print_attri: malloc failed\n"),
			progname);
		exit(1);
	}
	if (xfs_attri_copy_log_format((char*)src_f, src_len, f))
		goto out;

	if (xfs_attr_log_item_op(f) == XFS_ATTRI_OP_FLAGS_NVREPLACE) {
		name_len      = f->alfi_old_name_len;
		new_name_len  = f->alfi_new_name_len;
		value_len     = f->alfi_value_len;
		new_value_len = f->alfi_new_value_len;
	} else {
		name_len      = f->alfi_name_len;
		value_len     = f->alfi_value_len;
	}

	printf(_("ATTRI:  #regs: %d	f: 0x%x, ino: 0x%llx, attr_filter: 0x%x, name_len: %u, new_name_len: %u, value_len: %u, new_value_len: %u  id: 0x%llx\n"),
			f->alfi_size,
			f->alfi_op_flags,
			(unsigned long long)f->alfi_ino,
			f->alfi_attr_filter,
			name_len,
			new_name_len,
			value_len,
			new_value_len,
			(unsigned long long)f->alfi_id);

	if (name_len > 0) {
		region++;
		printf(_("ATTRI:  name len:%u\n"), name_len);
		print_or_dump((char *)item->ri_buf[region].i_addr,
			       name_len);
		name_ptr = item->ri_buf[region].i_addr;
	}

	if (new_name_len > 0) {
		region++;
		printf(_("ATTRI:  newname len:%u\n"), new_name_len);
		print_or_dump((char *)item->ri_buf[region].i_addr,
			       new_name_len);
		new_name_ptr = item->ri_buf[region].i_addr;
	}

	if (value_len > 0) {
		int	len = min(MAX_ATTR_VAL_PRINT, value_len);

		region++;
		printf(_("ATTRI:  value len:%u\n"), value_len);
		print_or_dump((char *)item->ri_buf[region].i_addr, len);
		value_ptr = item->ri_buf[region].i_addr;
	}

	if (new_value_len > 0) {
		int	len = min(MAX_ATTR_VAL_PRINT, new_value_len);

		region++;
		printf(_("ATTRI:  newvalue len:%u\n"), new_value_len);
		print_or_dump((char *)item->ri_buf[region].i_addr, len);
		new_value_ptr = item->ri_buf[region].i_addr;
	}

	if (src_f->alfi_attr_filter & XFS_ATTR_PARENT)
		dump_pptr_update(name_ptr, name_len,
				 new_name_ptr, new_name_len,
				 value_ptr, value_len,
				 new_value_ptr, new_value_len);

out:
	free(f);

}

int
xlog_print_trans_attrd(char **ptr, uint len)
{
	struct xfs_attrd_log_format *f;
	struct xfs_attrd_log_format lbuf;
	uint core_size = sizeof(struct xfs_attrd_log_format);

	memcpy(&lbuf, *ptr, MIN(core_size, len));
	f = &lbuf;
	*ptr += len;
	if (len >= core_size) {
		printf(_("ATTRD:  #regs: %d	id: 0x%llx\n"),
			f->alfd_size,
			(unsigned long long)f->alfd_alf_id);
		return 0;
	} else {
		printf(_("ATTRD: Not enough data to decode further\n"));
		return 1;
	}
}	/* xlog_print_trans_attrd */

void
xlog_recover_print_attrd(
	struct xlog_recover_item		*item)
{
	struct xfs_attrd_log_format	*f;

	f = (struct xfs_attrd_log_format *)item->ri_buf[0].i_addr;

	printf(_("	ATTRD:  #regs: %d	id: 0x%llx\n"),
		f->alfd_size,
		(unsigned long long)f->alfd_alf_id);
}

/* Atomic Extent Swapping Items */

static int
xfs_sxi_copy_format(
	struct xfs_sxi_log_format *sxi,
	uint			  len,
	struct xfs_sxi_log_format *dst_fmt,
	int			  continued)
{
	if (len == sizeof(struct xfs_sxi_log_format) || continued) {
		memcpy(dst_fmt, sxi, len);
		return 0;
	}
	fprintf(stderr, _("%s: bad size of SXI format: %u; expected %zu\n"),
		progname, len, sizeof(struct xfs_sxi_log_format));
	return 1;
}

int
xlog_print_trans_sxi(
	char			**ptr,
	uint			src_len,
	int			continued)
{
	struct xfs_sxi_log_format *src_f, *f = NULL;
	struct xfs_swap_extent	*ex;
	int			error = 0;

	src_f = malloc(src_len);
	if (src_f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	memcpy(src_f, *ptr, src_len);
	*ptr += src_len;

	/* convert to native format */
	if (continued && src_len < sizeof(struct xfs_sxi_log_format)) {
		printf(_("SXI: Not enough data to decode further\n"));
		error = 1;
		goto error;
	}

	f = malloc(sizeof(struct xfs_sxi_log_format));
	if (f == NULL) {
		fprintf(stderr, _("%s: %s: malloc failed\n"),
			progname, __func__);
		exit(1);
	}
	if (xfs_sxi_copy_format(src_f, src_len, f, continued)) {
		error = 1;
		goto error;
	}

	printf(_("SXI:  #regs: %d	num_extents: 1  id: 0x%llx\n"),
		f->sxi_size, (unsigned long long)f->sxi_id);

	if (continued) {
		printf(_("SXI extent data skipped (CONTINUE set, no space)\n"));
		goto error;
	}

	ex = &f->sxi_extent;
	printf("(ino1: 0x%llx, ino2: 0x%llx, off1: %lld, off2: %lld, len: %lld, flags: 0x%llx)\n",
		(unsigned long long)ex->sx_inode1,
		(unsigned long long)ex->sx_inode2,
		(unsigned long long)ex->sx_startoff1,
		(unsigned long long)ex->sx_startoff2,
		(unsigned long long)ex->sx_blockcount,
		(unsigned long long)ex->sx_flags);
error:
	free(src_f);
	free(f);
	return error;
}

void
xlog_recover_print_sxi(
	struct xlog_recover_item	*item)
{
	char				*src_f;
	uint				src_len;

	src_f = item->ri_buf[0].i_addr;
	src_len = item->ri_buf[0].i_len;

	xlog_print_trans_sxi(&src_f, src_len, 0);
}

int
xlog_print_trans_sxd(
	char				**ptr,
	uint				len)
{
	struct xfs_sxd_log_format	*f;
	struct xfs_sxd_log_format	lbuf;

	/* size without extents at end */
	uint core_size = sizeof(struct xfs_sxd_log_format);

	memcpy(&lbuf, *ptr, min(core_size, len));
	f = &lbuf;
	*ptr += len;
	if (len >= core_size) {
		printf(_("SXD:  #regs: %d	                 id: 0x%llx\n"),
			f->sxd_size,
			(unsigned long long)f->sxd_sxi_id);

		/* don't print extents as they are not used */

		return 0;
	} else {
		printf(_("SXD: Not enough data to decode further\n"));
		return 1;
	}
}

void
xlog_recover_print_sxd(
	struct xlog_recover_item	*item)
{
	char				*f;

	f = item->ri_buf[0].i_addr;
	xlog_print_trans_sxd(&f, sizeof(struct xfs_sxd_log_format));
}
