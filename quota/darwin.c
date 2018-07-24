// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "quota.h"
#include <sys/quota.h>

int
xfsquotactl(
	int		command,
	const char	*device,
	uint		type,
	uint		id,
	void		*addr)
{
	/* return quotactl(device, QCMD(command, type), id, addr); */
	errno = -ENOSYS;
	return -1;
}
