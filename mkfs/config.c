/*
 * Copyright (c) 2018 Luis R. Rodriguez <mcgrof@kernel.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>

#include "libxfs.h"
#include "config.h"

/*
 * Enums for each configuration option. All these currently match the CLI
 * parameters for now but this may change later, so we keep all this code
 * and definitions separate. The rules for configuration parameters may also
 * differ.
 *
 * We only provide definitions for what we currently support parsing.
 */

enum data_subopts {
	D_NOALIGN = 0,
};

enum inode_subopts {
	I_ALIGN = 0,
	I_PROJID32BIT,
	I_SPINODES,
};

enum log_subopts {
	L_LAZYSBCNTR = 0,
};

enum metadata_subopts {
	M_CRC = 0,
	M_FINOBT,
	M_RMAPBT,
	M_REFLINK,
};

enum naming_subopts {
	N_FTYPE = 0,
};

enum rtdev_subopts {
	R_NOALIGN = 0,
};

/* Just define the max options array size manually right now */
#define MAX_SUBOPTS	5

static int config_check_bool(
	uint64_t			value)
{
	if (value > 1)
		goto out;

	return 0;
out:
	errno = ERANGE;
	return -1;
}


static int
data_config_parser(
	struct mkfs_default_params	*dft,
	int				psubopt,
	uint64_t			value)
{
	enum data_subopts	subopt = psubopt;

	if (config_check_bool(value) != 0)
		return -1;

	switch (subopt) {
	case D_NOALIGN:
		dft->sb_feat.nodalign = value;
		return 0;
	}
	return -1;
}

static int
inode_config_parser(
	struct mkfs_default_params	*dft,
	int				psubopt,
	uint64_t			value)
{
	enum inode_subopts	subopt = psubopt;

	if (config_check_bool(value) != 0)
		return -1;

	switch (subopt) {
	case I_ALIGN:
		dft->sb_feat.inode_align = value;
		return 0;
	case I_PROJID32BIT:
		dft->sb_feat.projid32bit = value;
		return 0;
	case I_SPINODES:
		dft->sb_feat.spinodes = value;
		return 0;
	}
	return -1;
}

static int
log_config_parser(
	struct mkfs_default_params	*dft,
	int				psubopt,
	uint64_t			value)
{
	enum log_subopts	subopt = psubopt;

	if (config_check_bool(value) != 0)
		return -1;

	switch (subopt) {
	case L_LAZYSBCNTR:
		dft->sb_feat.lazy_sb_counters = value;
		return 0;
	}
	return -1;
}

static int
metadata_config_parser(
	struct mkfs_default_params	*dft,
	int				psubopt,
	uint64_t			value)
{
	enum metadata_subopts	subopt = psubopt;

	if (config_check_bool(value) != 0)
		return -1;

	switch (subopt) {
	case M_CRC:
		dft->sb_feat.crcs_enabled = value;
		if (dft->sb_feat.crcs_enabled)
			dft->sb_feat.dirftype = true;
		return 0;
	case M_FINOBT:
		dft->sb_feat.finobt = value;
		return 0;
	case M_RMAPBT:
		dft->sb_feat.rmapbt = value;
		return 0;
	case M_REFLINK:
		dft->sb_feat.reflink = value;
		return 0;
	}
	return -1;
}

static int
naming_config_parser(
	struct mkfs_default_params	*dft,
	int				psubopt,
	uint64_t			value)
{
	enum naming_subopts	subopt = psubopt;

	if (config_check_bool(value) != 0)
		return -1;

	switch (subopt) {
	case N_FTYPE:
		dft->sb_feat.dirftype = value;
		return 0;
	}
	return -1;
}

static int
rtdev_config_parser(
	struct mkfs_default_params	*dft,
	int				psubopt,
	uint64_t			value)
{
	enum rtdev_subopts	subopt = psubopt;

	if (config_check_bool(value) != 0)
		return -1;

	switch (subopt) {
	case R_NOALIGN:
		dft->sb_feat.nortalign = value;
		return 0;
	}
	return -1;
}

struct confopts {
	const char		*name;
	const char		*subopts[MAX_SUBOPTS];
	int			(*parser)(struct mkfs_default_params *dft,
					  int psubopt, uint64_t value);
	bool			seen;
} confopts_tab[] = {
	{
		.name = "data",
		.subopts = {
			[D_NOALIGN] = "noalign",
			NULL
		},
		.parser = data_config_parser,
	},
	{
		.name = "inode",
		.subopts = {
			[I_ALIGN] = "align",
			[I_PROJID32BIT] = "projid32bit",
			[I_SPINODES] = "sparse",
			NULL
		},
		.parser = inode_config_parser,
	},
	{
		.name = "log",
		.subopts = {
			[L_LAZYSBCNTR] = "lazy-count",
			NULL
		},
		.parser = log_config_parser,
	},
	{
		.name = "naming",
		.subopts = {
			[N_FTYPE] = "ftype",
			NULL
		},
		.parser = naming_config_parser,
	},
	{
		.name = "rtdev",
		.subopts = {
			[R_NOALIGN] = "noalign",
			NULL
		},
		.parser = rtdev_config_parser,
	},
	{
		.name = "metadata",
		.subopts = {
			[M_CRC] = "crc",
			[M_FINOBT] = "finobt",
			[M_RMAPBT] = "rmapbt",
			[M_REFLINK] = "reflink",
			NULL
		},
		.parser = metadata_config_parser,
	},
};

static struct confopts *
get_confopts(
	const char	*section)
{
	unsigned int	i;
	struct confopts	*opts;

	for (i=0; i < ARRAY_SIZE(confopts_tab); i++) {
		opts = &confopts_tab[i];
		if (strcmp(opts->name, section) == 0)
			return opts;
	}
	errno = EINVAL;
	return NULL;
}

enum parse_line_type {
	PARSE_COMMENT = 0,
	PARSE_EMPTY,
	PARSE_SECTION,
	PARSE_TAG_VALUE,
	PARSE_INVALID,
	PARSE_EOF,
};

static bool
isempty(
	const char	*line,
	ssize_t		linelen)
{
	ssize_t		i = 0;
	char		p;

	while (i < linelen) {
		p = line[i++];

		/* tab or space */
		if (!isblank(p))
			return false;
	}

	return true;
}

static bool
iscomment(
	const char	*line,
	ssize_t		linelen)
{
	ssize_t		i = 0;
	char		p;

	while (i != linelen) {
		p = line[i];
		i++;

		/* tab or space */
		if (isblank(p))
			continue;

		if (p == '#')
			return true;

		return false;
	}

	return false;
}

static enum parse_line_type
parse_get_line_type(
	const char	*line,
	ssize_t		linelen,
	char		**tag,
	uint64_t	*value)
{
	int		ret;
	uint64_t	u64_value;

	if (isempty(line, linelen))
		return PARSE_EMPTY;

	if (iscomment(line, linelen))
		return PARSE_COMMENT;

	/* check if we have a section header */
	ret = sscanf(line, " [%m[^]]]", tag);
	if (ret == 1)
		return  PARSE_SECTION;

	if (ret == EOF)
		return PARSE_EOF;

	/* should be a "tag = value" config option */
	ret = sscanf(line, " %m[^ \t=] = %" PRIu64 " ", tag, &u64_value);
	if (ret == 2) {
		*value = u64_value;

		return PARSE_TAG_VALUE;
	}

	if (ret == EOF)
		return PARSE_EOF;

	errno = EINVAL;
	return PARSE_INVALID;
}

static int
parse_config_stream(
	struct mkfs_default_params	*dft,
	const char 			*config_file,
	FILE				*fp)
{
	int				ret = -1;
	char				*line = NULL;
	ssize_t				linelen;
	size_t				len = 0, lineno = 0;
	uint64_t			value;
	enum parse_line_type		parse_type;
	struct confopts			*confopt = NULL;
	int				subopt;
	char *tag = NULL;

	while ((linelen = getline(&line, &len, fp)) != -1) {
		char *ignore_value;
		char *p;

		lineno++;

		/*
		 * tag is allocated for us by scanf(), it must freed only on any
		 * successful parse of a section or tag-value pair.
		 */
		parse_type = parse_get_line_type(line, linelen, &tag, &value);

		switch (parse_type) {
		case PARSE_EMPTY:
		case PARSE_COMMENT:
			/* Nothing tag to free for these */
			continue;
		case PARSE_EOF:
			break;
		case PARSE_INVALID:
			ret = -1;
			fprintf(stderr, _("Invalid line %s:%zu : %s\n"),
					  config_file, lineno, line);
			goto out;
		case PARSE_SECTION:
			confopt = get_confopts(tag);
			if (!confopt) {
				fprintf(stderr, _("Invalid section on line %s:%zu : %s\n"),
						config_file, lineno, tag);
				goto out_free_tag;
			}
			if (!confopt->subopts) {
				fprintf(stderr, _("Section not yet supported on line %s:%zu : %s\n"),
						config_file, lineno, tag);
				goto out_free_tag;
			}
			if (confopt->seen) {
				errno = EINVAL;
				fprintf(stderr, _("Section '%s' respecified\n"),
						tag);
				goto out_free_tag;
			}
			confopt->seen = true;
			free(tag);
			break;
		case PARSE_TAG_VALUE:
			if (!confopt) {
				fprintf(stderr, _("No section specified yet on line %s:%zu : %s\n"),
						config_file, lineno, line);
				goto out_free_tag;
			}

			/*
			 * We re-use the line buffer allocated by getline(),
			 * however line must be kept pointing to its original
			 * value to free it later. A separate pointer is needed
			 * as getsubopt() will otherwise muck with the value
			 * passed.
			 */
			p = line;

			/*
			 * Trims white spaces. getsubopt() does not grok
			 * white space, it would fail otherwise.
			 */
			snprintf(p, len, "%s=%lu", tag, value);

			/* Not needed anymore */
			free(tag);

			/*
			 * We only use getsubopt() to validate the possible
			 * subopt, we already parsed the value and its already
			 * in a more preferred data type.
			 */
			subopt = getsubopt(&p, (char **) confopt->subopts,
					   &ignore_value);

			ret = confopt->parser(dft, subopt, value);
			if (ret) {
				errno = EINVAL;
				fprintf(stderr, _("Error parsine line %s:%zu : %s\n"),
						config_file, lineno, line);
				goto out;
			}

			break;
		}
		free(line);
		line = NULL;
	}

out:
	/* We must free even if getline() failed */
	if (line)
		free(line);
	return ret;

out_free_tag:
	if (tag)
		free(tag);
	ret = -1;
	goto out;
}

static int
config_stat_check(
	struct stat		*sp)
{
	if (!S_ISREG(sp->st_mode)) {
		errno = EINVAL;
		return -1;
	}

	/* Anything beyond 1 MiB is kind of silly right now */
	if (sp->st_size > 1 * 1024 * 1024) {
		errno = E2BIG;
		return -1;
	}

	return 0;
}

/*
 * If the file is not found -1 is returned and errno set. Otherwise
 * the file descriptor is returned.
 */
int
open_cli_config(
	int			dirfd,
	const char		*cli_config_file,
	char			**fpath)
{
	int			fd = -1, len, ret;
	struct stat		st;

	fd = openat(AT_FDCWD, cli_config_file, O_NOFOLLOW, O_RDONLY);
	if (fd < 0) {
		len = snprintf(*fpath, PATH_MAX, "%s/%s", MKFS_XFS_CONF_DIR,
			       cli_config_file);
		/* Indicates truncation */
		if (len >= PATH_MAX) {
			errno = ENAMETOOLONG;
			goto out;
		}

		fd = openat(dirfd, cli_config_file, O_NOFOLLOW, O_RDONLY);
		if (fd < 0)
			goto out;

		ret = fstatat(dirfd, cli_config_file, &st,
			      AT_SYMLINK_NOFOLLOW);
		if (ret != 0)
			goto err_out_close;

		ret = config_stat_check(&st);
		if (ret != 0)
			goto err_out_close;

		goto out;
	}

	memcpy(*fpath, cli_config_file, strlen(cli_config_file));

	ret = fstatat(AT_FDCWD, cli_config_file, &st, AT_SYMLINK_NOFOLLOW);
	if (ret != 0)
		goto err_out_close;

	ret = config_stat_check(&st);
	if (ret != 0)
		goto err_out_close;
out:
	return fd;
err_out_close:
	close(fd);
	return -1;
}

#ifndef O_PATH
#if defined __alpha__
#define O_PATH		040000000
#elif defined(__hppa__)
#define O_PATH		020000000
#elif defined(__sparc__)
#define O_PATH		0x1000000
#else
#define O_PATH		010000000
#endif
#endif /* O_PATH */

int
open_config_file(
	const char			*cli_config_file,
	struct mkfs_default_params	*dft,
	char				**fpath)
{
	int			dirfd, fd = -1, len, ret;
	struct stat		st;

	*fpath = malloc(PATH_MAX);
	if (!*fpath)
		return -1;

	memset(*fpath, 0, PATH_MAX);

	dirfd = open(MKFS_XFS_CONF_DIR, O_PATH|O_NOFOLLOW|O_DIRECTORY);

	if (cli_config_file) {
		if (strlen(cli_config_file) > PATH_MAX) {
			errno = ENAMETOOLONG;
			goto out;
		}
		fd = open_cli_config(dirfd, cli_config_file, fpath);
		goto out;
	}

	fd = openat(dirfd, "default", O_NOFOLLOW, O_RDONLY);
	if (fd < 0)
		goto out;

	dft->type = DEFAULTS_CONFIG;

	len = snprintf(*fpath, PATH_MAX, "%s/%s", MKFS_XFS_CONF_DIR, "default");
	/* Indicates truncation */
	if (len >= PATH_MAX) {
		errno = ENAMETOOLONG;
		goto err_out_close;
	}

	ret = fstatat(dirfd, "default", &st, AT_SYMLINK_NOFOLLOW);
	if (ret != 0)
		goto err_out_close;

	ret = config_stat_check(&st);
	if (ret != 0)
		goto err_out_close;

out:
	if (fd < 0) {
		if (dft->type != DEFAULTS_BUILTIN) {
			fprintf(stderr, _("Unable to open %s config file: %s : %s\n"),
					default_type_str(dft->type), *fpath,
					strerror(errno));
			free(*fpath);
			exit(1);
		}
	}
	if (dirfd >= 0)
		close(dirfd);
	return fd;

err_out_close:
	close(fd);
	fd = -1;
	goto out;
}

/*
 * This is only called *iff* there is a configuration file which we know we
 * *must* parse.
 */
int
parse_defaults_file(
	int					fd,
	struct mkfs_default_params		*dft,
	const char				*config_file)
{
	FILE			*fp;
	int			ret;

	fp = fdopen(fd, "r");
	if (!fp)
		goto out;

	ret = parse_config_stream(dft, config_file, fp);
	if (ret) {
		fclose(fp);
		goto out;
	}

	printf(_("config-file=%s\n"), config_file);

	return 0;
out:
	return -1;
}
