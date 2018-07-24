// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2012 Red Hat, Inc.
 * All Rights Reserved.
 */
#ifndef XFS_SPACEMAN_SPACE_H_
#define XFS_SPACEMAN_SPACE_H_

typedef struct fileio {
	xfs_fsop_geom_t	geom;		/* XFS filesystem geometry */
	struct fs_path	fs_path;	/* XFS path information */
	char		*name;		/* file name at time of open */
	int		fd;		/* open file descriptor */
} fileio_t;

extern fileio_t		*filetable;	/* open file table */
extern int		filecount;	/* number of open files */
extern fileio_t		*file;		/* active file in file table */

extern int	openfile(char *, xfs_fsop_geom_t *, struct fs_path *);
extern int	addfile(char *, int , xfs_fsop_geom_t *, struct fs_path *);

extern void	print_init(void);
extern void	help_init(void);
extern void	prealloc_init(void);
extern void	quit_init(void);
extern void	trim_init(void);
#ifdef HAVE_GETFSMAP
extern void	freesp_init(void);
#else
# define freesp_init()	do { } while (0)
#endif
extern void	info_init(void);

#endif /* XFS_SPACEMAN_SPACE_H_ */
